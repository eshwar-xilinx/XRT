// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2015-2022 Xilinx, Inc. All rights reserved.
// Copyright (C) 2022 Advanced Micro Devices, Inc. All rights reserved.
#include "shim.h"
#include "system_swemu.h"
#include "pllauncher_defines.h"
#include "core/include/xclbin.h"
#include "core/common/xclbin_parser.h"
#include "core/common/api/hw_context_int.h"
#include <errno.h>
#include <unistd.h>
#include <boost/property_tree/xml_parser.hpp>

namespace xclcpuemhal2 {

  std::map<unsigned int, CpuemShim*> devices;
  unsigned int CpuemShim::mBufferCount = 0;
  unsigned int GraphType::mGraphHandle = 0;
  std::map<int, std::tuple<std::string,int,void*> > CpuemShim::mFdToFileNameMap;
  bool CpuemShim::mFirstBinary = true;
  const unsigned CpuemShim::TAG = 0X586C0C6C; // XL OpenCL X->58(ASCII), L->6C(ASCII), O->0 C->C L->6C(ASCII);
  const unsigned CpuemShim::CONTROL_AP_START = 1;
  const unsigned CpuemShim::CONTROL_AP_DONE  = 2;
  const unsigned CpuemShim::CONTROL_AP_IDLE  = 4;
  const unsigned CpuemShim::CONTROL_AP_CONTINUE = 0x10;
  std::map<std::string, std::string> CpuemShim::mEnvironmentNameValueMap(xclemulation::getEnvironmentByReadingIni());
#define PRINTENDFUNC if (mLogStream.is_open()) mLogStream << __func__ << " ended " << std::endl;

  bool isRemotePortMapped = false;
  void * remotePortMappedPointer = NULL;
  namespace pt = boost::property_tree;

  bool initRemotePortMap(std::string fpgaDevice)
  {
    int fd;
    unsigned addr;//, page_addr , page_offset;
    unsigned page_size = sysconf(_SC_PAGESIZE);

    fd = open("/dev/mem", O_RDWR);
    if (fd < 1) {
      std::cout << "Unable to open /dev/mem file" << std::endl;
      exit(-1);
    }

    if (fpgaDevice.find("versal:") != std::string::npos) {
      addr = PL_RP_VERSAL_ALLOCATED_ADD;
    }
    else if (fpgaDevice.find("zynquplus:") != std::string::npos) {
      addr = PL_RP_MP_ALLOCATED_ADD;
    }
    else {
      addr = PL_RP_ALLOCATED_ADD;
    }

    remotePortMappedPointer = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
      MAP_SHARED, fd, (addr & ~(page_size - 1)));

    if (remotePortMappedPointer == MAP_FAILED) {
      std::cout << "Remote Port mapping to address " << addr << " Failed" << std::endl;
      exit(-1);
    }
    xclcpuemhal2::isRemotePortMapped = true;
    return true;
  }

  bool validateXclBin(const xclBin *header, std::string &xclBinName, bool &deviceProcessInQemu , std::string &fpgaDevice)
  {
    char *bitstreambin = reinterpret_cast<char*> (const_cast<xclBin*> (header));
    //int result = 0; Not used. Removed to get rid of compiler warning, and probably a Coverity CID.
    ssize_t zipFileSize = 0;
    ssize_t xmlFileSize = 0;
    ssize_t debugFileSize = 0;
    ssize_t memTopologySize = 0;
    char* xmlFile = nullptr;

    if ((!std::memcmp(bitstreambin, "xclbin0", 7)) || (!std::memcmp(bitstreambin, "xclbin1", 7)))
    {
      return false;
    }
    else if (!std::memcmp(bitstreambin, "xclbin2", 7))
    {
      auto top = reinterpret_cast<const axlf*>(header);
      if (auto sec = xclbin::get_axlf_section(top, EMBEDDED_METADATA)) {
        xmlFileSize = sec->m_sectionSize;
        xmlFile = new char[xmlFileSize];
        memcpy(xmlFile, bitstreambin + sec->m_sectionOffset, xmlFileSize);
      }
    }
    else
    {
      return false;
    }

    if (!xmlFile)
    {
      return false;
    }

    pt::ptree xml_project;
    std::string sXmlFile;
    sXmlFile.assign(xmlFile, xmlFileSize);
    std::stringstream xml_stream;
    xml_stream << sXmlFile;
    pt::read_xml(xml_stream, xml_project);

    // iterate platforms
    int count = 0;
    for (auto& xml_platform : xml_project.get_child("project"))
    {
      if (xml_platform.first != "platform")
        continue;
      if (++count > 1)
      {
        //Give error and return from here
      }
    }
    // iterate devices
    count = 0;
    for (auto& xml_device : xml_project.get_child("project.platform"))
    {
      if (xml_device.first != "device")
        continue;
      fpgaDevice = xml_device.second.get<std::string>("<xmlattr>.fpgaDevice");
      if (++count > 1)
      {
        //Give error and return from here
      }
    }

    // iterate cores
    count = 0;
    for (auto& xml_core : xml_project.get_child("project.platform.device"))
    {
      if (xml_core.first != "core")
        continue;
      if (++count > 1)
      {
        //Give error and return from here
      }
    }
    xclBinName = xml_project.get<std::string>("project.<xmlattr>.name", "");
    // check for versal,zynqmp and zynq platforms. Enabling to run the device process in x86 instead of QEMU for all the Zynq, ZynqMP and Versal platforms
    if (fpgaDevice != "" && (fpgaDevice.find("versal:") != std::string::npos || fpgaDevice.find("zynq") != std::string::npos)) {
      deviceProcessInQemu = false;
    }
    return true;
  }

  CpuemShim::CpuemShim(unsigned int deviceIndex, xclDeviceInfo2 &info, std::list<xclemulation::DDRBank>& DDRBankList, bool _unified, bool _xpr, FeatureRomHeader& fRomHeader)
    :mTag(TAG)
    ,mRAMSize(info.mDDRSize)
    ,mCoalesceThreshold(4)
    ,mDeviceIndex(deviceIndex)
  {
    binaryCounter = 0;
    mReqCounter = 0;
    sock = nullptr;
    aiesim_sock = nullptr;
    ci_msg.set_size(0);
    ci_msg.set_xcl_api(0);
    mCore = nullptr;
    mSWSch = nullptr;

#if GOOGLE_PROTOBUF_VERSION < 3006001
    ci_buf = malloc(ci_msg.ByteSize());
#else
    ci_buf = malloc(ci_msg.ByteSizeLong());
#endif
    ri_msg.set_size(0);
#if GOOGLE_PROTOBUF_VERSION < 3006001
    ri_buf = malloc(ri_msg.ByteSize());
#else
    ri_buf = malloc(ri_msg.ByteSizeLong());
#endif
    buf = nullptr;
    buf_size = 0;

    deviceName = "device"+std::to_string(deviceIndex);
    deviceDirectory = xclemulation::getRunDirectory() + "/"+std::to_string(getpid())+"/sw_emu/"+deviceName;
    simulator_started = false;
    mVerbosity = XCL_INFO;

    std::memset(&mDeviceInfo, 0, sizeof(xclDeviceInfo2));
    fillDeviceInfo(&mDeviceInfo,&info);
    initMemoryManager(DDRBankList);

    std::memset(&mFeatureRom, 0, sizeof(FeatureRomHeader));
    std::memcpy(&mFeatureRom, &fRomHeader, sizeof(FeatureRomHeader));

    char* pack_size = getenv("SW_EMU_PACKET_SIZE");
    if(pack_size)
    {
      unsigned int messageSize = strtoll(pack_size,nullptr,0);
      message_size = messageSize;
    }
    else
    {
      message_size = 0x800000;
    }
    mCloseAll = false;
    bUnified = _unified;
    bXPR = _xpr;
    mIsKdsSwEmu = (xclemulation::is_sw_emulation()) ? xrt_core::config::get_flag_kds_sw_emu() : false;
    mDeviceProcessInQemu = true;
    mFpgaDevice = "";
  }

  size_t CpuemShim::alloc_void(size_t new_size)
  {
    if (buf_size == 0)
    {
      buf = malloc(new_size);
      return new_size;
    }
    if (buf_size < new_size)
    {
      void *temp = buf;
      buf = (void*) realloc(buf,new_size);
      if (!buf) // prevent leak of original buf
        free(temp);
      return new_size;
    }
    return buf_size;
  }


  void CpuemShim::initMemoryManager(std::list<xclemulation::DDRBank>& DDRBankList)
  {
    std::list<xclemulation::DDRBank>::iterator start = DDRBankList.begin();
    std::list<xclemulation::DDRBank>::iterator end = DDRBankList.end();
    uint64_t base = 0;
    for(;start != end; start++)
    {
      const uint64_t bankSize = (*start).ddrSize;
      mDdrBanks.push_back(*start);
      //CR 966701: alignment to 4k (instead of mDeviceInfo.mDataAlignment)
      mDDRMemoryManager.push_back(new xclemulation::MemoryManager(bankSize, base , getpagesize()));
      base += bankSize;
    }
  }

//private
  bool CpuemShim::isGood() const
  {
    // TODO: Add sanity check for card state
    return true;
  }

  CpuemShim *CpuemShim::handleCheck(void *handle)
  {
    // Sanity checks
    if (!handle)
      return 0;
    if (*(unsigned *)handle != TAG)
      return 0;
    if (!((CpuemShim *)handle)->isGood()) {
      return 0;
    }
    return (CpuemShim *)handle;
  }

  static void saveDeviceProcessOutputs()
  {
    std::map<unsigned int, CpuemShim*>::iterator start = devices.begin();
    std::map<unsigned int, CpuemShim*>::iterator end = devices.end();
    for(; start != end; start++)
    {
      CpuemShim* handle = (*start).second;
      if(!handle)
        continue;
      handle->saveDeviceProcessOutput();
    }

  }

  static void sigHandler(int sn, siginfo_t *si, void *sc)
  {
    switch (sn) {
    case SIGSEGV:
      saveDeviceProcessOutputs();
      kill(0, SIGSEGV);
      exit(1);
      break;
    case SIGFPE:
      saveDeviceProcessOutputs();
      kill(0, SIGTERM);
      exit(1);
      break;
    case SIGABRT:
      saveDeviceProcessOutputs();
      kill(0, SIGABRT);
      exit(1);
      break;
    case SIGCHLD: // Prevent infinite loop when the emulator dies
      if (si->si_code != CLD_KILLED && si->si_code != CLD_DUMPED)
        break;
    case SIGUSR1:
      // One of the spawned processes died for some reason,
      //  kill all of the others and exit the host code
      saveDeviceProcessOutputs();
      std::cerr << "Software emulation of compute unit(s) exited unexpectedly"
                << std::endl;
      kill(0, SIGTERM);
      exit(1);
      break;
    default:
      break;
    }
  }

  int CpuemShim::dumpXML(const xclBin* header, std::string& fileLocation)
  {
    if (!header) return 0 ; // We didn't dump it, but this isn't an error

    char* xclbininmemory =
      reinterpret_cast<char*>(const_cast<xclBin*>(header)) ;

    char* xmlfile = nullptr ;
    int xmllength = 0 ;

    if (memcmp(xclbininmemory, "xclbin0", 8) == 0)
    {
       if (mLogStream.is_open())
       {
	   mLogStream << __func__ << " unsupported Legacy XCLBIN header " << std::endl;
       }
       return -1;

      //xmlfile = xclbininmemory + (header->m_metadataOffset) ;
      //xmllength = (int)(header->m_metadataLength);
    }
    else if (memcmp(xclbininmemory,"xclbin2",7) == 0)
    {
      auto top = reinterpret_cast<const axlf*>(header);
      if (auto sec = xclbin::get_axlf_section(top,EMBEDDED_METADATA)) {
	xmlfile = xclbininmemory + sec->m_sectionOffset;
	xmllength = sec->m_sectionSize;
      }
    }
    else
    {
      // This was not a valid xclbin file
      if (mLogStream.is_open())
      {
	mLogStream << __func__ << " invalid XCLBIN header " << std::endl;
      }
      return -1 ;
    }

    if (xmlfile == nullptr || xmllength == 0)
    {
      // This xclbin file did not contain any XML meta-data
      if (mLogStream.is_open())
      {
	mLogStream << __func__ << " XCLBIN did not contain meta-data"
		   << std::endl ;
      }
      return -1 ;
    }

    // First, create the device directory if it doesn't exist
    systemUtil::makeSystemCall(deviceDirectory,
			       systemUtil::systemOperation::CREATE) ;
    // Second, create the binary directory if it doesn't exist
    std::stringstream binaryDirectory ;
    binaryDirectory << deviceDirectory << "/binary_" << binaryCounter ;
    std::string binDir = binaryDirectory.str() ;
    systemUtil::makeSystemCall(binDir,
			       systemUtil::systemOperation::CREATE) ;
    systemUtil::makeSystemCall(binDir,
			       systemUtil::systemOperation::PERMISSIONS,
			       "777") ;

    // The XML file will exist in this binary directory
    fileLocation = binDir + "/xmltmp" ;

    // Keep appending underscore to the file name until we find
    //  a file that does not exist.
    bool foundName = false ;
    while (!foundName)
    {
      FILE* fp = fopen(fileLocation.c_str(), "rb") ;
      if (fp == NULL)
      {
	// The file does not exist, so we can use this file location
	foundName = true ;
      }
      else
      {
	// The name we've chosen already exists, so append an underscore
	//  and try again
	fclose(fp) ;
	fileLocation += "_" ;
      }
    }

    // The file name we've chosen does not exist, so attempt to
    //  open it for writing
    FILE* fp = fopen(fileLocation.c_str(), "wb") ;
    if(fp==NULL)
    {
      if (mLogStream.is_open())
      {
	mLogStream << __func__ << " failed to create temporary xml file " << std::endl;
      }
      return -1;
    }
    fwrite(xmlfile,xmllength,1,fp);
    fflush(fp);
    fclose(fp);

    return 0 ;
  }

  bool CpuemShim::parseIni(unsigned int& debugPort)
  {
    debugPort = xclemulation::config::getInstance()->getServerPort() ;
    if (debugPort == 0)
    {
      return false ;
    }
    return true ;
  }

  void CpuemShim::launchDeviceProcess(bool debuggable, std::string& binaryDirectory)
  {
    std::lock_guard lk(mProcessLaunchMtx);
    systemUtil::makeSystemCall(deviceDirectory, systemUtil::systemOperation::CREATE);
    std::stringstream ss1;
    ss1<<deviceDirectory<<"/binary_"<<binaryCounter;
    binaryDirectory = ss1.str();
    systemUtil::makeSystemCall(binaryDirectory, systemUtil::systemOperation::CREATE);
    systemUtil::makeSystemCall(binaryDirectory, systemUtil::systemOperation::PERMISSIONS, "777");
    binaryCounter++;
    if(sock)
    {
      return;
    }

    struct sigaction s;
    memset(&s, 0, sizeof(s));
    s.sa_flags = SA_SIGINFO;
    s.sa_sigaction = sigHandler;
    if (sigaction(SIGSEGV, &s, (struct sigaction *)0) ||
        sigaction(SIGFPE , &s, (struct sigaction *)0) ||
        sigaction(SIGABRT, &s, (struct sigaction *)0) ||
        sigaction(SIGUSR1, &s, (struct sigaction *)0) ||
        sigaction(SIGCHLD, &s, (struct sigaction *)0))
    {
      //debug_print("unable to support all signals");
    }

    // We also need to check the .ini file in order to determine
    //  if the dynamic port on the sdx_server the child process
    //  must connect to was specified
    unsigned int debugPort = 0 ;
    bool passPort = parseIni(debugPort) ;
    std::stringstream portStream ;
    portStream << debugPort ;

    // If debuggable, the child process also requires the PID of the parent (us)
    pid_t parentPid = getpid() ;
    std::stringstream pidStream ;
    pidStream << parentPid ;

    // Spawn off the process to run the stub
    bool simDontRun = xclemulation::config::getInstance()->isDontRun();
    if(!simDontRun)
    {
      std::stringstream socket_id;
      socket_id << deviceName << "_" << binaryCounter << "_" << getpid();
      setenv("EMULATION_SOCKETID",socket_id.str().c_str(),true);

      pid_t pid = fork();
      assert(pid >= 0);
      if (pid == 0)
      {
        std::string childProcessPath("");
        std::string xilinxInstall("");

        //Added the latest ENV to get the install path
        char *vitisInstallEnvvar = getenv("XILINX_VITIS");
        if (vitisInstallEnvvar != NULL) {
          xilinxInstall = std::string(vitisInstallEnvvar);
        }

        char *scoutInstallEnvvar =  getenv("XILINX_SCOUT");
        if(scoutInstallEnvvar != NULL && xilinxInstall.empty() ){
          xilinxInstall = std::string(scoutInstallEnvvar);
        }

        char *installEnvvar = getenv("XILINX_SDX");
        if (installEnvvar != NULL && xilinxInstall.empty())
        {
          xilinxInstall = std::string(installEnvvar);
        }
        else
        {
          installEnvvar = getenv("XILINX_OPENCL");
          if (installEnvvar != NULL && xilinxInstall.empty())
          {
            xilinxInstall = std::string(installEnvvar);
          }
        }

        char *xilinxHLSEnvVar = getenv("XILINX_HLS");
        char *xilinxVivadoEnvVar = getenv("XILINX_VIVADO");

        if (xilinxHLSEnvVar && xilinxVivadoEnvVar)
        {
          std::string sHlsBinDir = xilinxHLSEnvVar;
          std::string sVivadoBinDir = xilinxVivadoEnvVar;

          std::string sLdLibs("");
          std::string DS("/");
          std::string sPlatform("lnx64");

          char* sLdLib = getenv("LD_LIBRARY_PATH");
          if (sLdLib)
            sLdLibs = std::string(sLdLib) + ":";

          sLdLibs += sHlsBinDir +  DS + sPlatform + DS + "tools" + DS + "fft_v9_1" + ":";
          sLdLibs += sHlsBinDir +  DS + sPlatform + DS + "tools" + DS + "fir_v7_0" + ":";
          sLdLibs += sHlsBinDir +  DS + sPlatform + DS + "tools" + DS + "fpo_v7_1" + ":";
          sLdLibs += sHlsBinDir +  DS + sPlatform + DS + "tools" + DS + "dds_v6_0" + ":";
          sLdLibs += sHlsBinDir +  DS + sPlatform + DS + "tools" + DS + "opencv"   + ":";
          sLdLibs += sHlsBinDir + DS + sPlatform + DS + "lib" + DS + "csim" + ":";
          sLdLibs += sHlsBinDir + DS + "lib" + DS + "lnx64.o" + DS + "Default" + DS + ":";
          sLdLibs += sHlsBinDir + DS + "lib" + DS + "lnx64.o" + DS + ":";
          sLdLibs += sVivadoBinDir + DS + "lib" + DS + "lnx64.o" + DS + ":";
          sLdLibs += sVivadoBinDir + DS + "lib" + DS + "lnx64.o" + DS + "Default" + DS;

          setenv("LD_LIBRARY_PATH",sLdLibs.c_str(),true);
        }

        if (xilinxInstall.empty()) {
          xilinxInstall = ".";
        }

        std::string modelDirectory("");
        modelDirectory = xilinxInstall + "/data/emulation/unified/cpu_em/zynqu/model/genericpciemodel";

#if defined(__aarch64__)
  modelDirectory = xilinxInstall + "/data/emulation/unified/cpu_em/zynqu/model/genericpciemodel";
#elif defined(__arm__)
  modelDirectory = xilinxInstall + "/data/emulation/unified/cpu_em/zynq/model/genericpciemodel";
#endif

        FILE *filep;
        if ((filep = fopen(modelDirectory.c_str(), "r")) != nullptr)
        {
          // file exists
          fclose(filep);
        }
        else
        {
          //File not found, no memory leak since 'file' == NULL
          std::cerr << "ERROR : [SW-EM 11] Unable to launch Device process, Please make sure that the XILINX_VITIS environment variable is set correctly" << std::endl;
          exit(1);
        }

        const char* childArgv[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr } ;
        childArgv[0] = modelDirectory.c_str() ;

        // If we determined this should be debuggable, pass the proper
        //  arguments to the process
        if (debuggable)
        {
          childArgv[1] = "-debug" ;
          childArgv[2] = "-ppid" ;
          childArgv[3] = pidStream.str().c_str() ;

          if (passPort)
          {
            childArgv[4] = "-port" ;
            childArgv[5] = portStream.str().c_str() ;
          }
        }
        int r = execl(modelDirectory.c_str(), childArgv[0], childArgv[1],
            childArgv[2], childArgv[3], childArgv[4], childArgv[5],
          nullptr) ;

        //fclose (stdout);
        if(r == -1){std::cerr << "FATAL ERROR : child process did not launch : " << modelDirectory  << std::endl; exit(1);}
        exit(0);
      }
    }
    sock = new unix_socket;
  }

  void CpuemShim::getCuRangeIdx() {
    for (const auto& kernel : m_xclbin.get_kernels()) {
      // get properties of each kernel object
      const auto& props = xrt_core::xclbin_int::get_properties(kernel);
      //get CU's of each kernel object.iterate over CU's to get arguments
      if (props.address_range != 0 && !props.name.empty())
        continue;
      for (const auto& cu : kernel.get_cus()) {
        auto instance_name = cu.get_name();
        if (!instance_name.empty())
          mCURangeMap.emplace(std::move(instance_name), props.address_range);
      }
    }
  }

  void CpuemShim::setDriverVersion(const std::string& version)
  {
    bool success = false;
    swemuDriverVersion_RPC_CALL(swemuDriverVersion, version);

    if (mLogStream.is_open())
      mLogStream << __func__ << " success " << success << std::endl;
  }

  int CpuemShim::xclLoadXclBin(const xclBin *header)
  {
    if (mLogStream.is_open()) mLogStream << __func__ << " begin " << std::endl;
    std::string xclBinName = "";
    if (!xclcpuemhal2::validateXclBin(header, xclBinName, mDeviceProcessInQemu, mFpgaDevice)) {
      printf("ERROR:Xclbin validation failed\n");
      return 1;
    }
    xclBinName = xclBinName + ".xclbin";
    if (mLogStream.is_open()) mLogStream << " validateXclBin done :  " << xclBinName << std::endl;

    if (!mDeviceProcessInQemu){
      return xclLoadXclBinNewFlow(header);
    }

    std::string xmlFile = "" ;
    int result = dumpXML(header, xmlFile) ;
    if (result != 0) return result ;
    // Before we spawn off the child process, we must determine
    //  if the process will be debuggable or not.  We get that
    //  by checking to see if there is a DEBUG_DATA section in
    //  the xclbin file.  Note, this only works with xclbin2
    //  files.  Also, the GUI can overwrite this by setting an
    //  environment variable
    bool debuggable = false ;
    if (getenv("ENABLE_KERNEL_DEBUG") != nullptr &&
	strcmp("true", getenv("ENABLE_KERNEL_DEBUG")) == 0)
    {
      char* xclbininmemory =
        reinterpret_cast<char*>(const_cast<xclBin*>(header)) ;
      if (!memcmp(xclbininmemory, "xclbin2", 7))
      {
        auto top = reinterpret_cast<const axlf*>(header) ;
        auto sec = xclbin::get_axlf_section(top, DEBUG_DATA) ;
        if (sec)
        {
          debuggable = true ;
        }
      }
    }

    std::string binaryDirectory("");
    launchDeviceProcess(debuggable,binaryDirectory);

    if(header)
    {
      resetProgram();
      std::string logFilePath = xrt_core::config::get_hal_logging();
      if (!logFilePath.empty()) {
        mLogStream.open(logFilePath);
        mLogStream << "FUNCTION, THREAD ID, ARG..." << std::endl;
        mLogStream << __func__ << ", " << std::this_thread::get_id() << std::endl;
      }

    if( mFirstBinary )
    {
      mFirstBinary = false;
    }

      char *xclbininmemory = reinterpret_cast<char*> (const_cast<xclBin*> (header));

      //parse header
      char *sharedlib = nullptr;
      int sharedliblength = 0;
      std::unique_ptr<char[]> memTopology;
      size_t memTopologySize = 0;
      std::unique_ptr<char[]> emuData;
      size_t emuDataSize = 0;
      std::unique_ptr<char[]> connectvitybuf;
      ssize_t connectvitybufsize = 0;

      //check header
      if (!memcmp(xclbininmemory, "xclbin0", 8))
      {
        if (mLogStream.is_open())
        {
          mLogStream << __func__ << " invalid XCLBIN header " << std::endl;
        }
        return -1;
      }
      else if (!memcmp(xclbininmemory,"xclbin2",7)) {
        auto top = reinterpret_cast<const axlf*>(header);
        m_xclbin = xrt::xclbin{top};
        if (auto sec = xclbin::get_axlf_section(top,BITSTREAM)) {
          sharedlib = xclbininmemory + sec->m_sectionOffset;
          sharedliblength = sec->m_sectionSize;
        }
        if (auto sec = xrt_core::xclbin::get_axlf_section(top, ASK_GROUP_TOPOLOGY)) {
          memTopologySize = sec->m_sectionSize;
          memTopology = std::unique_ptr<char[]>(new char[memTopologySize]);
          memcpy(memTopology.get(), xclbininmemory + sec->m_sectionOffset, memTopologySize);
        }
        //Extract EMULATION_DATA from XCLBIN
        if (auto sec = xrt_core::xclbin::get_axlf_section(top, EMULATION_DATA)) {
          emuDataSize = sec->m_sectionSize;
          emuData = std::unique_ptr<char[]>(new char[emuDataSize]);
          memcpy(emuData.get(), xclbininmemory + sec->m_sectionOffset, emuDataSize);
          getCuRangeIdx();
        }
	      //Extract CONNECTIVITY section from XCLBIN
        if (auto sec = xrt_core::xclbin::get_axlf_section(top, CONNECTIVITY)) {
          connectvitybufsize = sec->m_sectionSize;
          connectvitybuf = std::unique_ptr<char[]>(new char[connectvitybufsize]);
          memcpy(connectvitybuf.get(), xclbininmemory + sec->m_sectionOffset, connectvitybufsize);
        }
      }
      else
      {
        if (mLogStream.is_open())
        {
          mLogStream << __func__ << " invalid XCLBIN header " << std::endl;
          mLogStream << __func__ << " header " << xclbininmemory[0] << xclbininmemory[1] << xclbininmemory[2] <<  xclbininmemory[3] <<
            xclbininmemory[4] << xclbininmemory[5] << std::endl;
        }
        return -1;
      }
      //write out shared library to file for consumption with dlopen
      std::string tempdlopenfilename = binaryDirectory+"/dltmp";
      {
        bool tempfilecreated = false;
        unsigned int counter = 0;
        while( !tempfilecreated ) {
          FILE *fp = fopen(tempdlopenfilename.c_str(),"rb");
          if(fp== nullptr)
          {
            tempfilecreated = true;
          }
          else
          {
            fclose(fp);
            std::stringstream ss;
            ss << std::hex << counter;
            tempdlopenfilename+=ss.str();
            counter = counter+1;
          }
        }
        FILE *fp = fopen(tempdlopenfilename.c_str(),"wb");
        if( !fp )
        {
          if(mLogStream.is_open()) mLogStream << __func__ << " failed to create temporary dlopen file" << std::endl;
          return -1;
        }
        fwrite(sharedlib,sharedliblength,1,fp);
        fflush(fp);
        fclose(fp);
      }
      if (memTopology && connectvitybuf)
      {
        auto m_mem = (reinterpret_cast<const ::mem_topology*>(memTopology.get()));
        auto m_conn = (reinterpret_cast<const ::connectivity*>(connectvitybuf.get()));
        if (m_mem && m_conn)
        {
          //uint64_t argNum = 0;
          uint64_t prev_instanceBaseAddr = ULLONG_MAX;
          std::map<uint64_t, std::pair<uint64_t,std::string> > argFlowIdMap;
          for (int32_t conn_idx = 0; conn_idx<m_conn->m_count; ++conn_idx)
          {
            int32_t memdata_idx = m_conn->m_connection[conn_idx].mem_data_index;
            if (memdata_idx >(m_mem->m_count - 1))
              return -1;
            uint64_t route_id = m_mem->m_mem_data[memdata_idx].route_id;
            uint64_t arg_id = m_conn->m_connection[conn_idx].arg_index;
            uint64_t flow_id = m_mem->m_mem_data[memdata_idx].flow_id;//base address + flow_id combo
            uint64_t instanceBaseAddr = 0xFFFF0000 & flow_id;
            if (mLogStream.is_open())
              mLogStream << __func__ << " flow_id : " << flow_id << " route_id : " << route_id << " inst addr : " << instanceBaseAddr << " arg_id : " << arg_id << std::endl;
            if(prev_instanceBaseAddr != ULLONG_MAX && instanceBaseAddr != prev_instanceBaseAddr)
            {
              //RPC CALL
              bool success = false;
              xclSetupInstance_RPC_CALL(xclSetupInstance, prev_instanceBaseAddr , argFlowIdMap);

              if(mLogStream.is_open())
                mLogStream << __func__ << " setup instance: " << prev_instanceBaseAddr <<" success "<< success << std::endl;

              argFlowIdMap.clear();
              //argNum = 0;
            }
            if(m_mem->m_mem_data[memdata_idx].m_type == MEM_TYPE::MEM_STREAMING)
            {
              std::string m_tag (reinterpret_cast<const char*>(m_mem->m_mem_data[memdata_idx].m_tag));
              std::pair<uint64_t,std::string> mPair;
              mPair.first  = flow_id;
              mPair.second = m_tag;
              argFlowIdMap[arg_id] = mPair;
              //argFlowIdMap[argNum] = mPair;
            }
            //argNum++;
            prev_instanceBaseAddr = instanceBaseAddr;
          }
          bool success = false;
          xclSetupInstance_RPC_CALL(xclSetupInstance, prev_instanceBaseAddr, argFlowIdMap);

          if(mLogStream.is_open())
            mLogStream << __func__ << " setup instance: " << prev_instanceBaseAddr <<" success "<< success << std::endl;
        }
      }
      if (mIsKdsSwEmu)
      {
        mCore = new exec_core;
        mSWSch = new SWScheduler(this);
        mSWSch->init_scheduler_thread();
      }

      //Extract EMULATION_DATA from XCLBIN
      if (emuData && (emuDataSize > 1)) {
        std::string emuDataFilePath = binaryDirectory + "/emuDataFile";
        std::ofstream os(emuDataFilePath);
        os.write(emuData.get(), emuDataSize);
        systemUtil::makeSystemCall(emuDataFilePath, systemUtil::systemOperation::UNZIP, binaryDirectory, std::to_string(__LINE__));
        systemUtil::makeSystemCall(binaryDirectory, systemUtil::systemOperation::PERMISSIONS, "777", std::to_string(__LINE__));
      }

      bool ack = true;
      bool verbose = false;
      if(mLogStream.is_open())
        verbose = true;

      setDriverVersion("2.0");
      xclLoadBitstream_RPC_CALL(xclLoadBitstream,xmlFile,tempdlopenfilename,deviceDirectory,binaryDirectory,verbose);

      if(!ack)
        return -1;
    }
    return 0;
  }

   bool CpuemShim::isValidCu(uint32_t cu_index) {
    // get sorted cu addresses to match up with cu_index
    const auto& cuidx2addr = mCoreDevice->get_cus();
    if (cu_index >= cuidx2addr.size()) {
      std::string strMsg = "ERROR: [SW-EMU 20] invalid CU index: " + std::to_string(cu_index);
      mLogStream << __func__ << strMsg << std::endl;
      return false;
    }
    return true;
  }

  uint64_t CpuemShim::getCuAddRange(uint32_t cu_index) {
    uint64_t cuAddRange = 64 * 1024;
    for (const auto& cuInfo : mCURangeMap) {
      std::string instName = cuInfo.first;
      int cuIdx = static_cast<int>(cu_index);
      int tmpCuIdx = xclIPName2Index(instName.c_str());
      mLogStream << __func__ << " , instName :  " << instName  << " cuIdx : " << cuIdx << " tmpCuIdx: " << tmpCuIdx << std::endl;
      if (tmpCuIdx == cuIdx) {
        cuAddRange = cuInfo.second;
        mLogStream << __func__ << " , cuAddRange :  " << cuAddRange << std::endl;
      }
    }
    return cuAddRange;
  }

  bool CpuemShim::isValidOffset(uint32_t offset, uint64_t cuAddRange) {
    if (offset >= cuAddRange || (offset & (sizeof(uint32_t) - 1)) != 0) {
      std::string strMsg = "ERROR: [SW-EMU 21] xclRegRW - invalid CU offset: " + std::to_string(offset);
      mLogStream << __func__ << strMsg << std::endl;
      return false;
    }
    return true;
  }

  int CpuemShim::xclRegRW(bool rd, uint32_t cu_index, uint32_t offset, uint32_t *datap)
  {
    if (mLogStream.is_open())
      mLogStream << __func__ << ", " << std::this_thread::get_id() << ", " << "CU Idx : " << cu_index << " Offset : " << offset << " Datap : " << (*datap) << std::endl;

    if (!isValidCu(cu_index))
      return -EINVAL;

    const auto& cuidx2addr = mCoreDevice->get_cus();

    uint64_t cuAddRange = getCuAddRange(cu_index);

    if (!isValidOffset(offset, cuAddRange))
      return -EINVAL;

    uint64_t baseAddr = cuidx2addr[cu_index];
    size_t size = 4;
    if (rd) {
      xclRegRead_RPC_CALL(xclRegRead, baseAddr, offset, datap, size, 0, 0);
    } else {
      xclRegWrite_RPC_CALL(xclRegWrite, baseAddr, offset, datap, 0, 0);
    }
    return 0;
  }

  int CpuemShim::xclRegRead(uint32_t cu_index, uint32_t offset, uint32_t *datap)
  {
    if (mLogStream.is_open()) {
      mLogStream << __func__ << ", " << std::this_thread::get_id() << ", " << "CU Idx : " << cu_index << " Offset : " << offset << " Datap : " << (*datap)  << std::endl;
    }
    return xclRegRW(true, cu_index, offset, datap);
  }

  int CpuemShim::xclRegWrite(uint32_t cu_index, uint32_t offset, uint32_t data)
  {
    if (mLogStream.is_open()) {
      mLogStream << __func__ << ", " << std::this_thread::get_id() << ", " << "CU Idx : " << cu_index << " Offset : " << offset << " Datap : " << data << std::endl;
    }
    return xclRegRW(false, cu_index, offset, &data);
  }

  bool CpuemShim::isAieEnabled(const xclBin* header)
  {
    bool aieFlag = false;
    if (!header) return false; // We didn't dump it, but this isn't an error
    char* xclbininmemory = reinterpret_cast<char*>(const_cast<xclBin*>(header));

    if (memcmp(xclbininmemory, "xclbin0", 8) == 0) {
      if (mLogStream.is_open())
      {
        mLogStream << __func__ << " unsupported Legacy XCLBIN header " << std::endl;
      }
      return false;
    }
    else if (memcmp(xclbininmemory, "xclbin2", 7) == 0)
    {
      auto top = reinterpret_cast<const axlf*>(header);
      //Chech AIE_METADATA available in XCLBIN
      if (auto sec = xclbin::get_axlf_section(top, AIE_METADATA)) {
        if (mLogStream.is_open())
        {
          mLogStream << __func__ << "AIE_METADATA is available in XCLBIN  " << std::endl;
        }
        aieFlag = true;
      }
    }
    else
    {
      // This was not a valid xclbin file
      if (mLogStream.is_open())
      {
        mLogStream << __func__ << " invalid XCLBIN header " << std::endl;
      }
      return false;
    }
    return aieFlag;
  }

  void CpuemShim::socketConnection(bool isTCPSocket) {
    if (mLogStream.is_open()) mLogStream << __func__ << "TCP connection started" << std::endl;
    if (!sock)
      sock = new unix_socket(isTCPSocket);
    if (mLogStream.is_open()) mLogStream << __func__ << "TCP connection established" << std::endl;
  }

  int CpuemShim::xclLoadXclBinNewFlow(const xclBin *header)
  {
    if (mLogStream.is_open()) mLogStream << __func__ << " begin " << std::endl;

    // Before we spawn off the child process, we must determine
    //  if the process will be debuggable or not.  We get that
    //  by checking to see if there is a DEBUG_DATA section in
    //  the xclbin file.  Note, this only works with xclbin2
    //  files.  Also, the GUI can overwrite this by setting an
    //  environment variable
    std::string debugMode = "No";
    if (getenv("ENABLE_KERNEL_DEBUG") != nullptr &&
      strcmp("true", getenv("ENABLE_KERNEL_DEBUG")) == 0)
    {
      char* xclbininmemory =
        reinterpret_cast<char*>(const_cast<xclBin*>(header));
      if (!memcmp(xclbininmemory, "xclbin2", 7))
      {
        auto top = reinterpret_cast<const axlf*>(header);
        auto sec = xclbin::get_axlf_section(top, DEBUG_DATA);
        if (sec)
        {
          debugMode = "Yes";
        }
      }
    }
    //Create thread for TCP socket connection
    std::thread tcpSockThread = std::thread(&CpuemShim::socketConnection, this, true);

    bool simDontRun = xclemulation::config::getInstance()->isDontRun();
    if (!simDontRun) {
      if (!xclcpuemhal2::isRemotePortMapped) {
        xclcpuemhal2::initRemotePortMap(mFpgaDevice);
      }
      //Send the LoadXclBin
      PLLAUNCHER::OclCommand *cmd = new PLLAUNCHER::OclCommand();
      cmd->setCommand(PLLAUNCHER::PL_OCL_LOADXCLBIN_ID);
      cmd->addArg(debugMode.c_str());
      uint32_t length;
      uint8_t* buff = cmd->generateBuffer(&length);
      for (unsigned int i = 0; i < length; i += 4) {
        uint32_t copySize = (length - i) > 4 ? 4 : length - i;
        memcpy(((char*)(xclcpuemhal2::remotePortMappedPointer)) + i, buff + i, copySize);
      }
      //Send the end of packet
      char cPacketEndChar = PL_OCL_PACKET_END_MARKER;
      memcpy((char*)(xclcpuemhal2::remotePortMappedPointer), &cPacketEndChar, 1);
    }

    std::string xmlFile = "";
    std::string binaryDirectory("");
    //launchDeviceProcess(debuggable, binaryDirectory);
    systemUtil::makeSystemCall(deviceDirectory, systemUtil::systemOperation::CREATE);
    systemUtil::makeSystemCall(deviceDirectory, systemUtil::systemOperation::PERMISSIONS, "777");
    std::stringstream ss1;
    ss1 << deviceDirectory << "/binary_" << binaryCounter;
    binaryDirectory = ss1.str();
    systemUtil::makeSystemCall(binaryDirectory, systemUtil::systemOperation::CREATE);
    systemUtil::makeSystemCall(binaryDirectory, systemUtil::systemOperation::PERMISSIONS, "777");
    binaryCounter++;

    //Join tcp socket thread.
    if (tcpSockThread.joinable()) {
       tcpSockThread.join();
    }

    if (header)
    {
      resetProgram();
      std::string logFilePath = xrt_core::config::get_hal_logging();
      if (!mLogStream.is_open() && !logFilePath.empty()) {
        mLogStream.open(logFilePath);
        mLogStream << "FUNCTION, THREAD ID, ARG..." << std::endl;
        mLogStream << __func__ << ", " << std::this_thread::get_id() << std::endl;
      }

      if (mFirstBinary)
        mFirstBinary = false;
      char *xclbininmemory = reinterpret_cast<char*> (const_cast<xclBin*> (header));
      //parse header
      std::unique_ptr<char[]> memTopology;
      size_t memTopologySize = 0;
      std::unique_ptr<char[]> connectvitybuf;
      ssize_t connectvitybufsize = 0;
      char* xmlbuff = nullptr;
      size_t xmllength = 0;
      char *sharedlib = nullptr;
      size_t sharedliblength = 0;
      char* emuData;
      size_t emuDataSize = 0;

      //check header
      if (!memcmp(xclbininmemory, "xclbin0", 8))
      {
        if (mLogStream.is_open())
        {
          mLogStream << __func__ << " invalid XCLBIN header " << std::endl;
        }
        return -1;
      }
      else if (!memcmp(xclbininmemory, "xclbin2", 7)) {
        auto top = reinterpret_cast<const axlf*>(header);
        m_xclbin = xrt::xclbin{top};
        if (auto sec = xrt_core::xclbin::get_axlf_section(top, ASK_GROUP_TOPOLOGY)) {
          memTopologySize = sec->m_sectionSize;
          memTopology = std::unique_ptr<char[]>(new char[memTopologySize]);
          memcpy(memTopology.get(), xclbininmemory + sec->m_sectionOffset, memTopologySize);
        }
        //Extract CONNECTIVITY section from XCLBIN
        if (auto sec = xrt_core::xclbin::get_axlf_section(top, CONNECTIVITY)) {
          connectvitybufsize = sec->m_sectionSize;
          connectvitybuf = std::unique_ptr<char[]>(new char[connectvitybufsize]);
          memcpy(connectvitybuf.get(), xclbininmemory + sec->m_sectionOffset, connectvitybufsize);
        }
        //Extract BITSTREAM from XCLBIN
        if (auto sec = xrt_core::xclbin::get_axlf_section(top, BITSTREAM)) {
          sharedlib = xclbininmemory + sec->m_sectionOffset;
          sharedliblength = sec->m_sectionSize;
        }
        //Extract EMULATION_DATA from XCLBIN
        if (auto sec = xrt_core::xclbin::get_axlf_section(top, EMULATION_DATA)) {
          emuData = xclbininmemory + sec->m_sectionOffset;
          emuDataSize = sec->m_sectionSize;
          getCuRangeIdx();
        }
        //Extract EMBEDDED_METADATA from XCLBIN
        if (auto sec = xclbin::get_axlf_section(top, EMBEDDED_METADATA)) {
          xmlbuff = xclbininmemory + sec->m_sectionOffset;
          xmllength = sec->m_sectionSize;
        }
      }
      else
      {
        if (mLogStream.is_open())
        {
          mLogStream << __func__ << " invalid XCLBIN header " << std::endl;
          mLogStream << __func__ << " header " << xclbininmemory[0] << xclbininmemory[1] << xclbininmemory[2] << xclbininmemory[3] <<
            xclbininmemory[4] << xclbininmemory[5] << std::endl;
        }
        return -1;
      }

      if (mLogStream.is_open()) mLogStream << __func__ << "Extracted xclbin data" << std::endl;

      if (memTopology && connectvitybuf)
      {
        auto m_mem = (reinterpret_cast<const ::mem_topology*>(memTopology.get()));
        auto m_conn = (reinterpret_cast<const ::connectivity*>(connectvitybuf.get()));
        if (m_mem && m_conn)
        {
          //uint64_t argNum = 0;
          uint64_t prev_instanceBaseAddr = ULLONG_MAX;
          std::map<uint64_t, std::pair<uint64_t, std::string> > argFlowIdMap;
          for (int32_t conn_idx = 0; conn_idx<m_conn->m_count; ++conn_idx)
          {
            int32_t memdata_idx = m_conn->m_connection[conn_idx].mem_data_index;
            if (memdata_idx >(m_mem->m_count - 1))
              return -1;
            uint64_t route_id = m_mem->m_mem_data[memdata_idx].route_id;
            uint64_t arg_id = m_conn->m_connection[conn_idx].arg_index;
            uint64_t flow_id = m_mem->m_mem_data[memdata_idx].flow_id;//base address + flow_id combo
            uint64_t instanceBaseAddr = 0xFFFF0000 & flow_id;
            if (mLogStream.is_open())
              mLogStream << __func__ << " flow_id : " << flow_id << " route_id : " << route_id << " inst addr : " << instanceBaseAddr << " arg_id : " << arg_id << std::endl;
            if (prev_instanceBaseAddr != ULLONG_MAX && instanceBaseAddr != prev_instanceBaseAddr)
            {
              //RPC CALL
              bool success = false;
              xclSetupInstance_RPC_CALL(xclSetupInstance, prev_instanceBaseAddr, argFlowIdMap);

              if (mLogStream.is_open())
                mLogStream << __func__ << " setup instance: " << prev_instanceBaseAddr << " success " << success << std::endl;

              argFlowIdMap.clear();
              //argNum = 0;
            }
            if (m_mem->m_mem_data[memdata_idx].m_type == MEM_TYPE::MEM_STREAMING)
            {
              std::string m_tag(reinterpret_cast<const char*>(m_mem->m_mem_data[memdata_idx].m_tag));
              std::pair<uint64_t, std::string> mPair;
              mPair.first = flow_id;
              mPair.second = m_tag;
              argFlowIdMap[arg_id] = mPair;
              //argFlowIdMap[argNum] = mPair;
            }
            //argNum++;
            prev_instanceBaseAddr = instanceBaseAddr;
          }
          bool success = false;
          xclSetupInstance_RPC_CALL(xclSetupInstance, prev_instanceBaseAddr, argFlowIdMap);

          if (mLogStream.is_open())
            mLogStream << __func__ << " setup instance: " << prev_instanceBaseAddr << " success " << success << std::endl;
        }
      }
      if (mIsKdsSwEmu)
      {
        mCore = new exec_core;
        mSWSch = new SWScheduler(this);
        mSWSch->init_scheduler_thread();
      }
      if (mLogStream.is_open()) mLogStream << __func__ << " xclLoadXclbinContent_RPC_CALL : started " << std::endl;
      //Send xclbin content i.e. sharedlib, xclbin xml, emudata over tcp sockets. its scoped call
      {
        bool keepdirc = xclemulation::config::getInstance()->isKeepRunDirEnabled() ? true : false;
        bool ack = true;
        xclLoadXclbinContent_RPC_CALL(xclLoadXclbinContent, xmlbuff, xmllength, sharedlib, sharedliblength, emuData, emuDataSize, keepdirc);
        if (!ack) {
          std::cerr << "xclLoadXclbinContent_RPC_CALL falied" << std::endl;
          return -1;
        }
      }

      if (mLogStream.is_open()) mLogStream << __func__ << " xclLoadXclbinContent_RPC_CALL : done " << std::endl;

      bool ack = true;
      bool verbose = false;
      std::string tempdlopenfilename("");
      if (mLogStream.is_open())
        verbose = true;
      xclLoadBitstream_RPC_CALL(xclLoadBitstream, xmlFile, tempdlopenfilename, deviceDirectory, binaryDirectory, verbose);
      if (!ack)
        return -1;
    }
    return 0;
  }

  int CpuemShim::xclGetDeviceInfo2(xclDeviceInfo2 *info)
  {
    std::memset(info, 0, sizeof(xclDeviceInfo2));
    fillDeviceInfo(info,&mDeviceInfo);
    for (auto i : mDDRMemoryManager)
    {
      info->mDDRFreeSize += i->freeSize();
    }
    return 0;
  }

  void CpuemShim::launchTempProcess()
  {
    std::string binaryDirectory("");
    launchDeviceProcess(false,binaryDirectory);
    std::string xmlFile("");
    std::string tempdlopenfilename("");
    SHIM_UNUSED bool ack = true;
    bool verbose = false;
    if(mLogStream.is_open())
      verbose = true;
    xclLoadBitstream_RPC_CALL(xclLoadBitstream,xmlFile,tempdlopenfilename,deviceDirectory,binaryDirectory,verbose);
  }

  uint64_t CpuemShim::xclAllocDeviceBuffer(size_t size)
  {

    size_t requestedSize =  size;
    if (mLogStream.is_open()) {
      mLogStream << __func__ << ", " << std::this_thread::get_id() << ", " << size << std::endl;
    }
    if(!sock)
    {
      launchTempProcess();
    }

    if (size == 0)
      size = DDR_BUFFER_ALIGNMENT;

    uint64_t result = xclemulation::MemoryManager::mNull;
    for (auto i : mDDRMemoryManager) {
      result = i->alloc(size);
      if (result != xclemulation::MemoryManager::mNull)
        break;
    }
    bool ack = false;
    //   Memory Manager Has allocated aligned address,
	//   size contains alignement + original size requested.
	//   We are passing original size to device process for exact stats.
    bool noHostMemory = false;
    std::string sFileName("");
    xclAllocDeviceBuffer_RPC_CALL(xclAllocDeviceBuffer,result,requestedSize,noHostMemory);
    if(!ack)
    {
      PRINTENDFUNC;
      return 0;
    }
      PRINTENDFUNC;
    return result;
  }

  uint64_t CpuemShim::xclAllocDeviceBuffer2(size_t& size, xclMemoryDomains domain, unsigned flags,bool noHostMemory,std::string &sFileName)
  {
    if (mLogStream.is_open()) {
      mLogStream << __func__ <<" , "<<std::this_thread::get_id() << ", " << size <<", "<<domain<<", "<< flags <<std::endl;
    }
    if(!sock)
    {
      launchTempProcess();
    }

    //flags = flags % 32;
    if (domain != XCL_MEM_DEVICE_RAM)
    {
      return xclemulation::MemoryManager::mNull;
    }

    if (size == 0)
      size = DDR_BUFFER_ALIGNMENT;

    if (flags >= mDDRMemoryManager.size())
    {
      return xclemulation::MemoryManager::mNull;
    }

    uint64_t result = mDDRMemoryManager[flags]->alloc(size);

    if (result == xclemulation::MemoryManager::mNull) {
      auto ddrSize = mDDRMemoryManager[flags]->size();
      std::string ddrSizeStr = std::to_string(ddrSize);
      std::string initMsg = "ERROR: [SW-EM 12] OutOfMemoryError : Requested Global memory size exceeds DDR limit " + ddrSizeStr + " Bytes";
      std::cout << initMsg << std::endl;
      return result;
    }

    bool ack = false;
    //   Memory Manager Has allocated aligned address,
	//   size contains alignement + original size requested.
	//   We are passing original size to device process for exact stats.
    xclAllocDeviceBuffer_RPC_CALL(xclAllocDeviceBuffer,result,size,noHostMemory);

    if(!ack)
    {
      PRINTENDFUNC;
      return 0;
    }
    PRINTENDFUNC;
    return result;
  }

  void CpuemShim::xclFreeDeviceBuffer(uint64_t offset)
  {
    if (mLogStream.is_open()) {
      mLogStream << __func__ << ", " << std::this_thread::get_id() << ", " << offset << std::endl;
    }

    for (auto i : mDDRMemoryManager) {
      if (offset < i->start() + i->size()) {
        i->free(offset);
      }
    }
    bool ack = true;
    if(sock)
    {
      xclFreeDeviceBuffer_RPC_CALL(xclFreeDeviceBuffer,offset);
    }
    if(!ack)
    {
      PRINTENDFUNC;
      return;
    }
    PRINTENDFUNC;
    return;
  }

  size_t CpuemShim::xclWrite(xclAddressSpace space, uint64_t offset, const void *hostBuf, size_t size)
  {
    std::lock_guard lk(mApiMtx);
    if (mLogStream.is_open()) {
      mLogStream << __func__ << ", " << std::this_thread::get_id() << ", " << offset<<", "<<hostBuf<<", "<< size<<std::endl;
    }

    if(!sock)
      return size;

    if(space != XCL_ADDR_KERNEL_CTRL)
    {
      if (mLogStream.is_open()) mLogStream << "xclWrite called with xclAddressSpace != XCL_ADDR_KERNEL_CTRL " << std::endl;
      return -1;
    }

    if(size%4)
    {
      if (mLogStream.is_open()) mLogStream << "xclWrite only supports 32-bit writes" << std::endl;
      return -1;
    }

    fflush(stdout);
    xclWriteAddrKernelCtrl_RPC_CALL(xclWriteAddrKernelCtrl,space,offset,hostBuf,size,kernelArgsInfo,0,0);
    PRINTENDFUNC;
    return size;
  }

  size_t CpuemShim::xclRead(xclAddressSpace space, uint64_t offset, void *hostBuf, size_t size)
  {
    std::lock_guard lk(mApiMtx);
    if (mLogStream.is_open()) {
      mLogStream << __func__ << ", " << std::this_thread::get_id() << ", " << space << ", "
        << offset << ", " << hostBuf << ", " << size << std::endl;
    }

    if(!sock)
    {
      PRINTENDFUNC;
      return size;
    }

    if(space != XCL_ADDR_KERNEL_CTRL)
    {
      if (mLogStream.is_open()) mLogStream << "xclRead called with xclAddressSpace != XCL_ADDR_KERNEL_CTRL " << std::endl;
      PRINTENDFUNC;
      return -1;
    }
    if(size!=4)
    {
      if (mLogStream.is_open()) mLogStream << "xclRead called with size != 4 " << std::endl;
      PRINTENDFUNC;
      return -1;
    }
    xclReadAddrKernelCtrl_RPC_CALL(xclReadAddrKernelCtrl,space,offset,hostBuf,size,0,0);
    PRINTENDFUNC;
    return size;

  }



  size_t CpuemShim::xclCopyBufferHost2Device(uint64_t dest, const void *src, size_t size, size_t seek)
  {
    if (mLogStream.is_open()) {
      mLogStream << __func__ << ", " << std::this_thread::get_id() << ", " << dest << ", "
        << src << ", " << size << ", " << seek << std::endl;
    }

    if(!sock)
    {
      launchTempProcess();
    }
    src = (unsigned char*)src + seek;
    dest += seek;

    void *handle = this;

    unsigned int messageSize = get_messagesize();
    unsigned int c_size = messageSize;
    unsigned int processed_bytes = 0;
    while(processed_bytes < size){
      if((size - processed_bytes) < messageSize){
        c_size = size - processed_bytes;
      }else{
        c_size = messageSize;
      }

      void* c_src = (((unsigned char*)(src)) + processed_bytes);
      uint64_t c_dest = dest + processed_bytes;
#ifndef _WINDOWS
      uint32_t space =0;
      xclCopyBufferHost2Device_RPC_CALL(xclCopyBufferHost2Device,handle,c_dest,c_src,c_size,seek,space);
#endif
      processed_bytes += c_size;
    }
    return size;
  }


  size_t CpuemShim::xclCopyBufferDevice2Host(void *dest, uint64_t src, size_t size, size_t skip)
  {
    if (mLogStream.is_open()) {
      mLogStream << __func__ << ", " << std::this_thread::get_id() << ", " << dest << ", "
        << src << ", " << size << ", " << skip << std::endl;
    }
    dest = ((unsigned char*)dest) + skip;

    if(!sock)
    {
      launchTempProcess();
    }
    src += skip;
    void *handle = this;

    unsigned int messageSize = get_messagesize();
    unsigned int c_size = messageSize;
    unsigned int processed_bytes = 0;

    while(processed_bytes < size){
      if((size - processed_bytes) < messageSize){
        c_size = size - processed_bytes;
      }else{
        c_size = messageSize;
      }

      void* c_dest = (((unsigned char*)(dest)) + processed_bytes);
      uint64_t c_src = src + processed_bytes;
#ifndef _WINDOWS
      uint32_t space =0;
      xclCopyBufferDevice2Host_RPC_CALL(xclCopyBufferDevice2Host,handle,c_dest,c_src,c_size,skip,space);
#endif

      processed_bytes += c_size;
    }
    return size;

  }

  void CpuemShim::xclOpen(const char* logfileName)
  {
    xclemulation::config::getInstance()->populateEnvironmentSetup(mEnvironmentNameValueMap);

    std::string logFilePath = (logfileName && (logfileName[0] != '\0')) ? logfileName : xrt_core::config::get_hal_logging();
    if (!mLogStream.is_open() && !logFilePath.empty()) {
      mLogStream.open(logFilePath);
      mLogStream << "FUNCTION, THREAD ID, ARG..."  << std::endl;
      mLogStream << __func__ << ", " << std::this_thread::get_id() << std::endl;
    }
    // Shim object creation doesn't follow xclOpen/xclClose.
    // The core device must correspond to open and close, so
    // create here rather than in constructor
    mCoreDevice = xrt_core::swemu::get_userpf_device(this, mDeviceIndex);
  }

  void CpuemShim::fillDeviceInfo(xclDeviceInfo2* dest, xclDeviceInfo2* src)
  {
    std::strcpy(dest->mName, src->mName);
    dest->mMagic               =    src->mMagic ;
    dest->mHALMajorVersion    =    src->mHALMajorVersion;
    dest->mHALMinorVersion    =    src->mHALMinorVersion;
    dest->mVendorId           =    src->mVendorId;
    dest->mDeviceId           =    src->mDeviceId;
    dest->mSubsystemVendorId  =    src->mSubsystemVendorId;
    dest->mDeviceVersion      =    src->mDeviceVersion;
    dest->mDDRSize            =    src->mDDRSize;
    dest->mDataAlignment      =    src->mDataAlignment;
    dest->mDDRBankCount       =    src->mDDRBankCount;
    for(unsigned int i = 0; i < 4 ;i++)
      dest->mOCLFrequency[i]       =    src->mOCLFrequency[i];
  }

  void CpuemShim::saveDeviceProcessOutput()
  {
    if(!sock)
      return;

    for(int i = binaryCounter-1; i >= 0; i--)
    {
      std::stringstream sw_emu_folder;
      sw_emu_folder <<deviceDirectory<<"/binary_"<<i;
      char path[FILENAME_MAX];
      size_t size = PATH_MAX;
      char* pPath = GetCurrentDir(path,size);
      if(pPath)
      {
        std::string debugFilePath = sw_emu_folder.str()+"/genericpcieoutput";
        std::string destPath = std::string(path) + "/genericpcieoutput_device"+ std::to_string(mDeviceIndex) + "_"+std::to_string(i);
        systemUtil::makeSystemCall(debugFilePath, systemUtil::systemOperation::COPY,destPath);
      }
    }
  }
  void CpuemShim::resetProgram(bool callingFromClose)
  {
    for (auto& it: mFdToFileNameMap)
    {
      int fd=it.first;
      int sSize = std::get<1>(it.second);
      void* addr = std::get<2>(it.second);
      munmap(addr,sSize);
      close(fd);
    }
    mFdToFileNameMap.clear();

    if (mLogStream.is_open()) {
      mLogStream << __func__ << ", " << std::this_thread::get_id() << std::endl;
    }
    if (!sock) {
      PRINTENDFUNC
      if (mIsKdsSwEmu && mSWSch && mCore)
      {
        mSWSch->fini_scheduler_thread();
        delete mCore;
        mCore = nullptr;
        delete mSWSch;
        mSWSch = nullptr;
      }
      return;
    }

    std::string socketName = sock->get_name();
    if(socketName.empty() == false)// device is active if socketName is non-empty
    {
#ifndef _WINDOWS
      xclClose_RPC_CALL(xclClose,this);
#endif
    }
   saveDeviceProcessOutput();
  }

  void CpuemShim::xclClose()
  {
    std::lock_guard lk(mApiMtx);
    if (mLogStream.is_open()) {
      mLogStream << __func__ << ", " << std::this_thread::get_id() << std::endl;
    }

    // Shim object is not deleted as part of closing device.
    // The core device must correspond to open and close, so
    // reset here rather than in destructor
    mCoreDevice.reset();

    if(!sock)
    {
      if( xclemulation::config::getInstance()->isKeepRunDirEnabled() == false)
        systemUtil::makeSystemCall(deviceDirectory, systemUtil::systemOperation::REMOVE);
      if (mIsKdsSwEmu && mSWSch && mCore)
      {
        mSWSch->fini_scheduler_thread();
        delete mCore;
        mCore = nullptr;
        delete mSWSch;
        mSWSch = nullptr;
      }
      return;
    }
    for (auto& it: mFdToFileNameMap)
    {
      int fd=it.first;
      int sSize = std::get<1>(it.second);
      void* addr = std::get<2>(it.second);
      munmap(addr,sSize);
      close(fd);
    }
      mFdToFileNameMap.clear();
    mCloseAll = true;
    std::string socketName = sock->get_name();
    if(socketName.empty() == false)// device is active if socketName is non-empty
    {
#ifndef _WINDOWS
      xclClose_RPC_CALL(xclClose,this);
#endif
    }
    mCloseAll = false;

    int status = 0;
    bool simDontRun = xclemulation::config::getInstance()->isDontRun();
    if(!simDontRun && mDeviceProcessInQemu)
      while (-1 == waitpid(0, &status, 0));

    systemUtil::makeSystemCall(socketName, systemUtil::systemOperation::REMOVE);
    delete sock;
    sock = nullptr;
    PRINTENDFUNC;
    if (mIsKdsSwEmu && mSWSch && mCore)
    {
      mSWSch->fini_scheduler_thread();
      delete mCore;
      mCore = nullptr;
      delete mSWSch;
      mSWSch = nullptr;
    }
    //clean up directories which are created inside the driver
    if( xclemulation::config::getInstance()->isKeepRunDirEnabled() == false)
    {
      //TODO sleeping for some time sothat gdb releases the process and its contents
      sleep(5);
      systemUtil::makeSystemCall(deviceDirectory, systemUtil::systemOperation::REMOVE);
    }
    google::protobuf::ShutdownProtobufLibrary();
  }

  CpuemShim::~CpuemShim()
  {
    if (mIsKdsSwEmu && mSWSch && mCore)
    {
      mSWSch->fini_scheduler_thread();
      delete mCore;
      mCore = nullptr;
      delete mSWSch;
      mSWSch = nullptr;
    }
    if (mLogStream.is_open())
    {
      mLogStream << __func__ << ", " << std::this_thread::get_id() << std::endl;
    }
    free(ci_buf);
    free(ri_buf);
    free(buf);

    if (mLogStream.is_open())
    {
      mLogStream << __func__ << ", " << std::this_thread::get_id() << std::endl;
      mLogStream.close();
    }
    //Tell the Pllauncher to close
    if (xclcpuemhal2::isRemotePortMapped) {
      auto cmd = std::make_unique<PLLAUNCHER::OclCommand>();
      cmd->setCommand(PLLAUNCHER::PL_OCL_XRESET_ID);
      uint32_t iLength;
      memcpy((char*)(xclcpuemhal2::remotePortMappedPointer), (char*)cmd->generateBuffer(&iLength), iLength);
      //Send the end of packet
      char cPacketEndChar = PL_OCL_PACKET_END_MARKER;
      memcpy((char*)(xclcpuemhal2::remotePortMappedPointer), &cPacketEndChar, 1);
    }
  }

  /**********************************************HAL2 API's START HERE **********************************************/

/*********************************** Utility ******************************************/

xclemulation::drm_xocl_bo* CpuemShim::xclGetBoByHandle(unsigned int boHandle)
{
  auto it = mXoclObjMap.find(boHandle);
  if(it == mXoclObjMap.end())
    return nullptr;

  xclemulation::drm_xocl_bo* bo = (*it).second;
  return bo;
}

inline unsigned short CpuemShim::xocl_ddr_channel_count()
{
  return mDeviceInfo.mDDRBankCount;
}

inline unsigned long long CpuemShim::xocl_ddr_channel_size()
{
  return 0;
}

int CpuemShim::xclGetBOProperties(unsigned int boHandle, xclBOProperties *properties)
{
  std::lock_guard lk(mApiMtx);
  if (mLogStream.is_open())
  {
    mLogStream << __func__ << ", " << std::this_thread::get_id() << ", " << std::hex << boHandle << std::endl;
  }
  xclemulation::drm_xocl_bo* bo = xclGetBoByHandle(boHandle);
  if (!bo) {
    PRINTENDFUNC;
    return  -1;
  }
  properties->handle = bo->handle;
  properties->flags  = bo->flags;
  properties->size   = bo->size;
  properties->paddr  = bo->base;
  PRINTENDFUNC;
  return 0;
}
/*****************************************************************************************/

/******************************** xclAllocBO *********************************************/
uint64_t CpuemShim::xoclCreateBo(xclemulation::xocl_create_bo* info)
{
  size_t size = info->size;
  unsigned ddr = xclemulation::xocl_bo_ddr_idx(info->flags);

  if (!size)
    return -1;

  // system linker doesnt run in sw_emu. if ddr idx morethan ddr_count, then create it in 0 by considering all plrams in zero'th ddr
	const unsigned ddr_count = xocl_ddr_channel_count();
  if(ddr_count <= ddr)
  {
    ddr = 0;
  }

  //struct xclemulation::drm_xocl_bo *xobj = new xclemulation::drm_xocl_bo;
  auto xobj = std::make_unique<xclemulation::drm_xocl_bo>();
  xobj->flags=info->flags;
  /* check whether buffer is p2p or not*/
  bool noHostMemory = xclemulation::no_host_memory(xobj.get()) || xclemulation::xocl_bo_host_only(xobj.get());
  std::string sFileName("");
  xobj->base = xclAllocDeviceBuffer2(size,XCL_MEM_DEVICE_RAM,ddr,noHostMemory,sFileName);
  xobj->filename = sFileName;
  xobj->size = size;
  xobj->userptr = nullptr;
  xobj->buf = nullptr;
  xobj->fd = -1;

  if (xobj->base == xclemulation::MemoryManager::mNull)
  {
    return xclemulation::MemoryManager::mNull;
  }

  info->handle = mBufferCount;
  mXoclObjMap[mBufferCount++] = xobj.release();
  return 0;
}

unsigned int CpuemShim::xclAllocBO(size_t size, int unused, unsigned flags)
{
  std::lock_guard lk(mApiMtx);
  if (mLogStream.is_open())
  {
    mLogStream << __func__ << ", " << std::this_thread::get_id() << ", " << std::hex << size << std::dec << " , "<< unused <<" , "<< flags << std::endl;
  }
  xclemulation::xocl_create_bo info = {size, mNullBO, flags};
  uint64_t result = xoclCreateBo(&info);
  PRINTENDFUNC;
  return result ? mNullBO : info.handle;
}
/***************************************************************************************/

/******************************** xclAllocUserPtrBO ************************************/
unsigned int CpuemShim::xclAllocUserPtrBO(void *userptr, size_t size, unsigned flags)
{
  std::lock_guard lk(mApiMtx);
  if (mLogStream.is_open())
  {
    mLogStream << __func__ << ", " << std::this_thread::get_id() << ", " << userptr <<", " << std::hex << size << std::dec <<" , "<< flags << std::endl;
  }
  xclemulation::xocl_create_bo info = {size, mNullBO, flags};
  uint64_t result = xoclCreateBo(&info);
  xclemulation::drm_xocl_bo* bo = xclGetBoByHandle(info.handle);
  if (bo) {
    bo->userptr = userptr;
  }
  PRINTENDFUNC;
  return result ? mNullBO : info.handle;
}
/***************************************************************************************/

/******************************** xclExportBO *******************************************/
int CpuemShim::xclExportBO(unsigned int boHandle)
{
  if (mLogStream.is_open())
  {
    mLogStream << __func__ << ", " << std::this_thread::get_id() << ", " << std::hex << boHandle << std::endl;
  }
  xclemulation::drm_xocl_bo* bo = xclGetBoByHandle(boHandle);
  if(!bo)
    return -1;

  std::string sFileName = bo->filename;
  if(sFileName.empty())
  {
    std::cout<<"Exported Buffer is not P2P "<<std::endl;
    PRINTENDFUNC;
    return -1;
  }

  uint64_t size = bo->size;
  int fd = open(sFileName.c_str(), (O_CREAT | O_RDWR), 0666);
  if (fd == -1)
  {
    printf("Error opening exported BO file.\n");
    PRINTENDFUNC;
    return -1;
  };

  char* data = (char*) mmap(0, bo->size , PROT_READ |PROT_WRITE |PROT_EXEC ,  MAP_SHARED, fd, 0);
  if(!data)
  {
    PRINTENDFUNC;
    return -1;
  }

  int fR = ftruncate(fd, bo->size);
  if(fR == -1)
  {
    close(fd);
    munmap(data,bo->size);
    return -1;
  }
  mFdToFileNameMap [fd] = std::make_tuple(sFileName,size,(void*)data);
  PRINTENDFUNC;
  return fd;
}
/***************************************************************************************/

/******************************** xclImportBO *******************************************/
unsigned int CpuemShim::xclImportBO(int boGlobalHandle, unsigned flags)
{
  //TODO
  if (mLogStream.is_open())
  {
    mLogStream << __func__ << ", " << std::this_thread::get_id() << ", " << std::hex << boGlobalHandle << std::endl;
  }
  auto itr = mFdToFileNameMap.find(boGlobalHandle);
  if(itr != mFdToFileNameMap.end())
  {
    const std::string& fileName = std::get<0>((*itr).second);
    int size = std::get<1>((*itr).second);
    unsigned int importedBo = xclAllocBO(size, 0,flags);
    xclemulation::drm_xocl_bo* bo = xclGetBoByHandle(importedBo);
    if(!bo)
    {
      std::cout<<"ERROR HERE in importBO "<<std::endl;
      return -1;
    }
    mImportedBOs.insert(importedBo);
    bo->fd = boGlobalHandle;
    bool ack;
    xclImportBO_RPC_CALL(xclImportBO,fileName,bo->base,size);
    if(!ack)
      return -1;
    PRINTENDFUNC;
    return importedBo;
  }
  return -1;
}
/***************************************************************************************/

/******************************** xclCopyBO *******************************************/
int CpuemShim::xclCopyBO(unsigned int dst_boHandle, unsigned int src_boHandle, size_t size, size_t dst_offset, size_t src_offset)
{
  std::lock_guard lk(mApiMtx);
  //TODO
  if (mLogStream.is_open())
  {
    mLogStream << __func__ << ", " << std::this_thread::get_id() << ", " << std::hex << dst_boHandle
      <<" , "<< src_boHandle << " , "<< size <<"," << dst_offset << "," <<src_offset<< std::endl;
  }
  xclemulation::drm_xocl_bo* sBO = xclGetBoByHandle(src_boHandle);
  if(!sBO)
  {
    PRINTENDFUNC;
    return -1;
  }

  xclemulation::drm_xocl_bo* dBO = xclGetBoByHandle(dst_boHandle);
  if(!dBO)
  {
    PRINTENDFUNC;
    return -1;
  }
  if(dBO->fd < 0)
  {
    std::cout<<"bo is not exported for copying"<<std::endl;
    return -1;
  }

  int ack = false;
  auto fItr = mFdToFileNameMap.find(dBO->fd);
  if(fItr != mFdToFileNameMap.end())
  {
    const std::string& sFileName = std::get<0>((*fItr).second);
    xclCopyBO_RPC_CALL(xclCopyBO,sBO->base,sFileName,size,src_offset,dst_offset);
  }
  if(!ack)
    return -1;
  PRINTENDFUNC;
  return 0;
}
/***************************************************************************************/

/******************************** xclMapBO *********************************************/
void *CpuemShim::xclMapBO(unsigned int boHandle, bool write)
{
  std::lock_guard lk(mApiMtx);
  if (mLogStream.is_open())
  {
    mLogStream << __func__ << ", " << std::this_thread::get_id() << ", " << std::hex << boHandle << " , " << write << std::endl;
  }
  xclemulation::drm_xocl_bo* bo = xclGetBoByHandle(boHandle);
  if (!bo) {
    PRINTENDFUNC;
    return nullptr;
  }

  std::string sFileName = bo->filename;
  if(!sFileName.empty() )
  {
    int fd = open(sFileName.c_str(), (O_CREAT | O_RDWR), 0666);
    if (fd == -1)
    {
      printf("Error opening exported BO file.\n");
      return nullptr;
    };

    char* data = (char*) mmap(0, bo->size , PROT_READ |PROT_WRITE |PROT_EXEC ,  MAP_SHARED, fd, 0);
    if(!data)
      return nullptr;

    int fR = ftruncate(fd, bo->size);
    if(fR == -1)
    {
      close(fd);
      munmap(data,bo->size);
      return nullptr;
    }
    mFdToFileNameMap [fd] = std::make_tuple(sFileName,bo->size,(void*)data);
    bo->buf = data;
    PRINTENDFUNC;
    return data;
  }

  void *pBuf=nullptr;
  if (posix_memalign(&pBuf, getpagesize(), bo->size))
  {
    if (mLogStream.is_open()) mLogStream << "posix_memalign failed" << std::endl;
    pBuf=nullptr;
  }
  bo->buf = pBuf;
  PRINTENDFUNC;
  return pBuf;
}

int CpuemShim::xclUnmapBO(unsigned int boHandle, void* addr)
{
  std::lock_guard lk(mApiMtx);
  auto bo = xclGetBoByHandle(boHandle);
  return bo ? munmap(addr,bo->size) : -1;
}

/**************************************************************************************/

/******************************** xclSyncBO *******************************************/
int CpuemShim::xclSyncBO(unsigned int boHandle, xclBOSyncDirection dir, size_t size, size_t offset)
{
  std::lock_guard lk(mApiMtx);
  if (mLogStream.is_open())
  {
    mLogStream << __func__ << ", " << std::this_thread::get_id() << ", " << std::hex << boHandle << " , " << std::endl;
  }
  xclemulation::drm_xocl_bo* bo = xclGetBoByHandle(boHandle);
  if(!bo)
  {
    PRINTENDFUNC;
    return -1;
  }

  int returnVal = 0;
  if(dir == XCL_BO_SYNC_BO_TO_DEVICE)
  {
    void* buffer =  bo->userptr ? bo->userptr : bo->buf;
    if (xclCopyBufferHost2Device(bo->base,buffer, size, offset) != size) {
      returnVal = EIO;
    }
  }
  else
  {
    void* buffer =  bo->userptr ? bo->userptr : bo->buf;
    if (xclCopyBufferDevice2Host(buffer, bo->base, size, offset) != size) {
      returnVal = EIO;
    }
  }
  PRINTENDFUNC;
  return returnVal;
}
/***************************************************************************************/

/******************************** xclFreeBO *******************************************/
void CpuemShim::xclFreeBO(unsigned int boHandle)
{
  std::lock_guard lk(mApiMtx);
  if (mLogStream.is_open())
  {
    mLogStream << __func__ << ", " << std::this_thread::get_id() << ", " << std::hex << boHandle << std::endl;
  }
  auto it = mXoclObjMap.find(boHandle);
  if(it == mXoclObjMap.end())
  {
    PRINTENDFUNC;
    return;
  }
  xclemulation::drm_xocl_bo* bo = (*it).second;;
  if(bo)
  {
    xclFreeDeviceBuffer(bo->base);
    mXoclObjMap.erase(it);
  }
  PRINTENDFUNC;
}
/***************************************************************************************/

/******************************** xclWriteBO *******************************************/
size_t CpuemShim::xclWriteBO(unsigned int boHandle, const void *src, size_t size, size_t seek)
{
  std::lock_guard lk(mApiMtx);
  if (mLogStream.is_open())
  {
    mLogStream << __func__ << ", " << std::this_thread::get_id() << ", " << std::hex << boHandle << " , "<< src <<" , "<< size << ", " << seek << std::endl;
  }
  xclemulation::drm_xocl_bo* bo = xclGetBoByHandle(boHandle);
  if(!bo)
  {
    PRINTENDFUNC;
    return -1;
  }
  size_t returnVal = 0;
  if (xclCopyBufferHost2Device(bo->base, src, size, seek) != size) {
    returnVal = EIO;
  }
  PRINTENDFUNC;
  return returnVal;
}
/***************************************************************************************/

/******************************** xclReadBO *******************************************/
size_t CpuemShim::xclReadBO(unsigned int boHandle, void *dst, size_t size, size_t skip)
{
  std::lock_guard lk(mApiMtx);
  if (mLogStream.is_open())
  {
    mLogStream << __func__ << ", " << std::this_thread::get_id() << ", " << std::hex << boHandle << " , "<< dst <<" , "<< size << ", " << skip << std::endl;
  }
  xclemulation::drm_xocl_bo* bo = xclGetBoByHandle(boHandle);
  if(!bo)
  {
    PRINTENDFUNC;
    return -1;
  }
  size_t returnVal = 0;
  if (xclCopyBufferDevice2Host(dst, bo->base, size, skip) != size) {
    returnVal = EIO;
  }
  PRINTENDFUNC;
  return returnVal;
}

/*
 * xclLogMsg()
 */
int CpuemShim::xclLogMsg(xclDeviceHandle handle, xrtLogMsgLevel level, const char* tag, const char* format, va_list args1)
{
    int len = std::vsnprintf(nullptr, 0, format, args1);

    if (len < 0)
    {
        //illegal arguments
        std::string err_str = "ERROR: Illegal arguments in log format string. ";
        err_str.append(std::string(format));
        xrt_core::message::send((xrt_core::message::severity_level)level, tag, err_str.c_str());
        return len;
    }
    len++; //To include null terminator

    std::vector<char> buf(len);
    len = std::vsnprintf(buf.data(), len, format, args1);

    if (len < 0)
    {
        //error processing arguments
        std::string err_str = "ERROR: When processing arguments in log format string. ";
        err_str.append(std::string(format));
        xrt_core::message::send((xrt_core::message::severity_level)level, tag, err_str.c_str());
        return len;
    }
    xrt_core::message::send((xrt_core::message::severity_level)level, tag, buf.data());
    return 0;
}

/*
* xclOpenContext
*/
int CpuemShim::xclOpenContext(const uuid_t xclbinId, unsigned int ipIndex, bool shared)
{
  // When properly implemented this function must throw on error
  // and any exception must be caught by global xclOpenContext and
  // converted to error code
  return 0;
}

/*
* xclExecWait
*/
int CpuemShim::xclExecWait(int timeoutMilliSec)
{
  //unsigned int tSec = 0;
  //static bool bConfig = true;
  //tSec = timeoutMilliSec / 1000;
  //if (bConfig)
  //{
  //  tSec = timeoutMilliSec / 1000;
  //  bConfig = false;
  //}
  //sleep(tSec);
  //PRINTENDFUNC;
  return 1;
}

/*
* xclExecBuf
*/
int CpuemShim::xclExecBuf(unsigned int cmdBO)
{
  if (mLogStream.is_open())
  {
    mLogStream << __func__ << ", " << std::this_thread::get_id() << ", " << cmdBO << std::endl;
  }

  if (!mIsKdsSwEmu)
    return 0;

  xclemulation::drm_xocl_bo* bo = xclGetBoByHandle(cmdBO);
  if (!mSWSch || !bo)
  {
    PRINTENDFUNC;
    return -1;
  }
  int ret = mSWSch->add_exec_buffer(mCore, bo);
  PRINTENDFUNC;
  return ret;
}

/*
* xclCloseContext
*/
int CpuemShim::xclCloseContext(const uuid_t xclbinId, unsigned int ipIndex)
{
  return 0;
}

//Get CU index from IP_LAYOUT section for corresponding kernel name
int CpuemShim::xclIPName2Index(const char *name)
{
  //Get IP_LAYOUT buffer from xclbin
  auto buffer = mCoreDevice->get_axlf_section(IP_LAYOUT);
  return xclemulation::getIPName2Index(name, buffer.first);
}

// open_context() - aka xclOpenContextByName
xrt_core::cuidx_type
CpuemShim::
open_cu_context(const xrt::hw_context& hwctx, const std::string& cuname)
{
  // Edge does not yet support multiple xclbins.  Call
  // regular flow.  Default access mode to shared unless explicitly
  // exclusive.
  auto shared = (hwctx.get_mode() != xrt::hw_context::access_mode::exclusive);
  auto ctxhdl = static_cast<xcl_hwctx_handle>(hwctx);
  auto cuidx = mCoreDevice->get_cuidx(ctxhdl, cuname);
  xclOpenContext(hwctx.get_xclbin_uuid().get(), cuidx.index, shared);

  return cuidx;
}

void
CpuemShim::
close_cu_context(const xrt::hw_context& hwctx, xrt_core::cuidx_type cuidx)
{
  // To-be-implemented
  if (xclCloseContext(hwctx.get_xclbin_uuid().get(), cuidx.index))
    throw xrt_core::system_error(errno, "failed to close cu context (" + std::to_string(cuidx.index) + ")");
}

/******************************* XRT Graph API's **************************************************/
/**
* xrtGraphInit() - Initialize  graph
*/
int CpuemShim::xrtGraphInit(void * gh) {

  std::lock_guard lk(mApiMtx);
  bool ack = false;
  auto ghPtr = (xclcpuemhal2::GraphType*)gh;
  if (!ghPtr)
    return -1;
  auto graphhandle = ghPtr->getGraphHandle();
  auto graphname = ghPtr->getGraphName();
  xclGraphInit_RPC_CALL(xclGraphInit, graphhandle, graphname);
  if (!ack)
  {
    PRINTENDFUNC;
    return -1;
  }
  return 0;
}

/**
* xrtGraphRun() - Start a graph execution
*/
int CpuemShim::xrtGraphRun(void * gh, uint32_t iterations) {

  std::lock_guard lk(mApiMtx);
  bool ack = false;
  auto ghPtr = (xclcpuemhal2::GraphType*)gh;
  if (!ghPtr)
    return -1;
  auto graphhandle = ghPtr->getGraphHandle();
  xclGraphRun_RPC_CALL(xclGraphRun, graphhandle, iterations);
  if (!ack)
  {
    PRINTENDFUNC;
    return -1;
  }
  return 0;
}

/**
* xrtGraphWait() -  Wait a given AIE cycle since the last xrtGraphRun and
*                   then stop the graph. If cycle is 0, busy wait until graph
*                   is done. If graph already run more than the given
*                   cycle, stop the graph immediateley.
*/
int CpuemShim::xrtGraphWait(void * gh) {

  std::lock_guard lk(mApiMtx);
  bool ack = false;
  auto ghPtr = (xclcpuemhal2::GraphType*)gh;
  if (!ghPtr)
    return -1;
  auto graphhandle = ghPtr->getGraphHandle();
  xclGraphWait_RPC_CALL(xclGraphWait, graphhandle);
  if (!ack)
  {
    PRINTENDFUNC;
    return -1;
  }
  return 0;
}

/**
* xrtGraphTimedWait() -  Wait a given AIE cycle since the last xrtGraphRun and
*                   then stop the graph. If cycle is 0, busy wait until graph
*                   is done. If graph already run more than the given
*                   cycle, stop the graph immediateley.
*/
int CpuemShim::xrtGraphTimedWait(void * gh, uint64_t cycle) {

  std::lock_guard lk(mApiMtx);
  bool ack = false;
  auto ghPtr = (xclcpuemhal2::GraphType*)gh;
  if (!ghPtr)
    return -1;
  auto graphhandle = ghPtr->getGraphHandle();
  xclGraphTimedWait_RPC_CALL(xclGraphTimedWait, graphhandle, cycle);
  if (!ack)
  {
    PRINTENDFUNC;
    return -1;
  }
  return 0;
}

/**
* xrtGraphEnd() - Wait a given AIE cycle since the last xrtGraphRun and
*                 then end the graph. If cycle is 0, busy wait until graph
*                 is done before end the graph. If graph already run more
*                 than the given cycle, stop the graph immediately and end it.
*
* @gh:              Handle to graph previously opened with xrtGraphOpen.
* @cycle:           AIE cycle should wait since last xrtGraphRun. 0 for
*                   wait until graph is done.
*
* Return:          0 on success, -1 on timeout.
*
* Note: This API with non-zero AIE cycle is for graph that is running
* forever or graph that has multi-rate core(s).
*/
int CpuemShim::xrtGraphEnd(void * gh) {

  uint32_t ack = false;

  auto ghPtr = (xclcpuemhal2::GraphType*)gh;
  if (!ghPtr)
    return -1;

  auto graphhandle = ghPtr->getGraphHandle();

  // ack = 0 : defines RPC Call is completed with failure status
  // ack = 1 : defines RPC Call is completed with success status
  // ack = 2 : defines RPC Call is returned with running status.
  // Recalling the RPC after a wait if the ack returned is 2.
  do
  {
    {
      std::lock_guard lk(mApiMtx);
      xclGraphEnd_RPC_CALL(xclGraphEnd, graphhandle);
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  } while (ack == 2);

  if (ack == 0)
  {
    PRINTENDFUNC;
    return -1;
  }
  return 0;
}

/**
* xrtGraphTimedEnd() - Wait a given AIE cycle since the last xrtGraphRun and
*                 then end the graph. If cycle is 0, busy wait until graph
*                 is done before end the graph. If graph already run more
*                 than the given cycle, stop the graph immediately and end it.
*
* @gh:              Handle to graph previously opened with xrtGraphOpen.
* @cycle:           AIE cycle should wait since last xrtGraphRun. 0 for
*                   wait until graph is done.
*
* Return:          0 on success, -1 on timeout.
*
* Note: This API with non-zero AIE cycle is for graph that is running
* forever or graph that has multi-rate core(s).
*/
int CpuemShim::xrtGraphTimedEnd(void * gh , uint64_t cycle) {
  std::lock_guard lk(mApiMtx);
  bool ack = false;
  auto ghPtr = (xclcpuemhal2::GraphType*)gh;
  if (!ghPtr)
    return -1;
  auto graphhandle = ghPtr->getGraphHandle();
  xclGraphTimedEnd_RPC_CALL(xclGraphTimedEnd, graphhandle, cycle);
  if (!ack)
  {
    PRINTENDFUNC;
    return -1;
  }
  return 0;
}

/**
* xrtGraphResume() - Resume a suspended graph.
*
* Resume graph execution which was paused by suspend() or wait(cycles) APIs
*/
int CpuemShim::xrtGraphResume(void * gh) {
  std::lock_guard lk(mApiMtx);
  bool ack = false;
  auto ghPtr = (xclcpuemhal2::GraphType*)gh;
  if (!ghPtr)
    return -1;
  auto graphhandle = ghPtr->getGraphHandle();
  xclGraphResume_RPC_CALL(xclGraphResume, graphhandle);
  if (!ack)
  {
    PRINTENDFUNC;
    return -1;
  }
  return 0;
}

/**
* xrtGraphUpdateRTP() - Update RTP value of port with hierarchical name
*
* @gh:              Handle to graph previously opened with xrtGraphOpen.
* @hierPathPort:    hierarchial name of RTP port.
* @buffer:          pointer to the RTP value.
* @size:            size in bytes of the RTP value.
*
* Return:          0 on success, -1 on error.
*/
int CpuemShim::xrtGraphUpdateRTP(void * gh, const char *hierPathPort, const char *buffer, size_t size) {
  std::lock_guard lk(mApiMtx);
  auto ghPtr = (xclcpuemhal2::GraphType*)gh;
  if (!ghPtr)
    return -1;
  auto graphhandle = ghPtr->getGraphHandle();
  xclGraphUpdateRTP_RPC_CALL(xclGraphUpdateRTP, graphhandle, hierPathPort, buffer, size);
  PRINTENDFUNC
    return 0;
}

/**
* xrtGraphUpdateRTP() - Read RTP value of port with hierarchical name
*
* @gh:              Handle to graph previously opened with xrtGraphOpen.
* @hierPathPort:    hierarchial name of RTP port.
* @buffer:          pointer to the buffer that RTP value is copied to.
* @size:            size in bytes of the RTP value.
*
* Return:          0 on success, -1 on error.
*
* Note: Caller is reponsible for allocating enough memory for RTP value
*       being copied to.
*/
int CpuemShim::xrtGraphReadRTP(void * gh, const char *hierPathPort, char *buffer, size_t size) {
  std::lock_guard lk(mApiMtx);
  auto ghPtr = (xclcpuemhal2::GraphType*)gh;
  if (!ghPtr)
    return -1;
  auto graphhandle = ghPtr->getGraphHandle();
  xclGraphReadRTP_RPC_CALL(xclGraphReadRTP, graphhandle, hierPathPort, buffer, size);
  PRINTENDFUNC
    return 0;
}


/**
* xrtSyncBOAIENB() - Transfer data between DDR and Shim DMA channel
*
* @bo:           BO obj.
* @gmioName:        GMIO port name
* @dir:             GM to AIE or AIE to GM
* @size:            Size of data to synchronize
* @offset:          Offset within the BO
*
* Return:          0 on success, or appropriate error number.
*
* Synchronize the buffer contents between GMIO and AIE.
* Note: Upon return, the synchronization is submitted or error out
*/

int CpuemShim::xrtSyncBOAIENB(xrt::bo& bo, const char *gmioname, enum xclBOSyncDirection dir, size_t size, size_t offset)
{
  bool ack = false;
  if (!gmioname)
    return -1;

  if (mLogStream.is_open())
    mLogStream << __func__ << ", bo.address() " << bo.address() << std::endl;

  auto boBase = bo.address();
  xclSyncBOAIENB_RPC_CALL(xclSyncBOAIENB, gmioname, dir, size, offset, boBase);
  if (!ack) {
    PRINTENDFUNC;
    return -1;
  }
  return 0;
}


/**
* xrtGMIOWait() - Wait a shim DMA channel to be idle for a given GMIO port
*
* @gmioName:        GMIO port name
*
* Return:          0 on success, or appropriate error number.
*/
int CpuemShim::xrtGMIOWait(const char *gmioname)
{
  bool ack = false;
  if (!gmioname)
    return -1;
  xclGMIOWait_RPC_CALL(xclGMIOWait, gmioname);
  if (!ack) {
    PRINTENDFUNC;
    return -1;
  }
  return 0;
}

/******************************* XRT Graph API's End here**************************************************/
/**********************************************HAL2 API's END HERE **********************************************/
}
