// Copyright (c) 2010, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-443211. All Rights
// reserved. See file COPYRIGHT for details.
//
// This file is part of the MFEM library. For more information and source code
// availability see http://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License (as published by the Free
// Software Foundation) version 2.1 dated February 1999.

#include "forall.hpp"
#include "cuda.hpp"
#include "occa.hpp"
#include "ceed.hpp"

#include <string>
#include <map>

namespace mfem
{

// Place the following variables in the mfem::internal namespace, so that they
// will not be included in the doxygen documentation.
namespace internal
{

#ifdef MFEM_USE_OCCA
// Default occa::device used by MFEM.
occa::device occaDevice;
#endif

#ifdef MFEM_USE_CEED
Ceed ceed;
#endif

// Backends listed by priority, high to low:
static const Backend::Id backend_list[Backend::NUM_BACKENDS] =
{
   Backend::OCCA_CUDA, Backend::RAJA_CUDA, Backend::CEED_CUDA, Backend::CUDA,
   Backend::OCCA_OMP, Backend::RAJA_OMP, Backend::OMP,
   Backend::OCCA_CPU, Backend::RAJA_CPU, Backend::CEED_CPU, Backend::CPU
};

// Backend names listed by priority, high to low:
static const char *backend_name[Backend::NUM_BACKENDS] =
{
   "occa-cuda", "raja-cuda", "ceed-cuda", "cuda", "occa-omp", "raja-omp", "omp",
   "occa-cpu", "raja-cpu", "ceed-cpu", "cpu"
};

} // namespace mfem::internal


// Initialize the unique global Device variable.
Device Device::device_singleton;


Device::~Device()
{
   if (destroy_mm) {
      free(ceed_option);
      mm.Destroy();
   }
}

void Device::Configure(const std::string &device, const int dev)
{
   std::map<std::string, Backend::Id> bmap;
   for (int i = 0; i < Backend::NUM_BACKENDS; i++)
   {
      bmap[internal::backend_name[i]] = internal::backend_list[i];
   }
   std::string::size_type beg = 0, end, option;
   while (1)
   {
      end = device.find(',', beg);
      end = (end != std::string::npos) ? end : device.size();
      const std::string bname = device.substr(beg, end - beg);
      option = bname.find(':');
      if (option==std::string::npos)//No option
      {
         const std::string backend = bname;
         std::map<std::string, Backend::Id>::iterator it = bmap.find(backend);
         MFEM_VERIFY(it != bmap.end(), "invalid backend name: '" << backend << '\'');
         Get().MarkBackend(it->second);
      }
      else
      {
         const std::string backend = bname.substr(0, option);
         const std::string boption = bname.substr(option+1);
         Get().ceed_option = strdup(boption.c_str());
         std::map<std::string, Backend::Id>::iterator it = bmap.find(backend);
         MFEM_VERIFY(it != bmap.end(), "invalid backend name: '" << backend << '\'');
         Get().MarkBackend(it->second);
         std::cout <<"libCEED backend: "<< boption << std::endl;
      }
      if (end == device.size()) { break; }
      beg = end + 1;
   }

   // OCCA_CUDA needs CUDA or RAJA_CUDA:
   if (Allows(Backend::OCCA_CUDA) && !Allows(Backend::RAJA_CUDA))
   {
      Get().MarkBackend(Backend::CUDA);
   }
   if (Allows(Backend::CEED_CUDA))
   {
      Get().MarkBackend(Backend::CUDA);
   }

   // Perform setup.
   Get().Setup(dev);

   // Enable the device
   Enable();

   // Copy all data members from the global 'singleton_device' into '*this'.
   std::memcpy(this, &Get(), sizeof(Device));

   // Only '*this' will call the MemoryManager::Destroy() method.
   destroy_mm = true;
}

void Device::Print(std::ostream &out)
{
   out << "Device configuration: ";
   bool add_comma = false;
   for (int i = 0; i < Backend::NUM_BACKENDS; i++)
   {
      if (backends & internal::backend_list[i])
      {
         if (add_comma) { out << ','; }
         add_comma = true;
         out << internal::backend_name[i];
      }
   }
   out << '\n';
}

void Device::UpdateMemoryTypeAndClass()
{
   if (Device::Allows(Backend::CUDA_MASK))
   {
      mem_type = MemoryType::CUDA;
      mem_class = MemoryClass::CUDA;
   }
   else
   {
      mem_type = MemoryType::HOST;
      mem_class = MemoryClass::HOST;
   }
}

void Device::Enable()
{
   if (Get().backends & ~Backend::CPU)
   {
      Get().mode = Device::ACCELERATED;
      Get().UpdateMemoryTypeAndClass();
   }
}

#ifdef MFEM_USE_CUDA
static void DeviceSetup(const int dev, int &ngpu)
{
   MFEM_CUDA_CHECK(cudaGetDeviceCount(&ngpu));
   MFEM_VERIFY(ngpu > 0, "No CUDA device found!");
   MFEM_CUDA_CHECK(cudaSetDevice(dev));
}
#endif

static void CudaDeviceSetup(const int dev, int &ngpu)
{
#ifdef MFEM_USE_CUDA
   DeviceSetup(dev, ngpu);
#endif
}

static void RajaDeviceSetup(const int dev, int &ngpu)
{
#ifdef MFEM_USE_CUDA
   if (ngpu <= 0) { DeviceSetup(dev, ngpu); }
#endif
}

static void OccaDeviceSetup(const int dev)
{
#ifdef MFEM_USE_OCCA
   const int cpu  = Device::Allows(Backend::OCCA_CPU);
   const int omp  = Device::Allows(Backend::OCCA_OMP);
   const int cuda = Device::Allows(Backend::OCCA_CUDA);
   if (cpu + omp + cuda > 1)
   {
      MFEM_ABORT("Only one OCCA backend can be configured at a time!");
   }
   if (cuda)
   {
#if OCCA_CUDA_ENABLED
      std::string mode("mode: 'CUDA', device_id : ");
      internal::occaDevice.setup(mode.append(1,'0'+dev));
#else
      MFEM_ABORT("the OCCA CUDA backend requires OCCA built with CUDA!");
#endif
   }
   else if (omp)
   {
#if OCCA_OPENMP_ENABLED
      internal::occaDevice.setup("mode: 'OpenMP'");
#else
      MFEM_ABORT("the OCCA OpenMP backend requires OCCA built with OpenMP!");
#endif
   }
   else
   {
      internal::occaDevice.setup("mode: 'Serial'");
   }

   std::string mfemDir;
   if (occa::io::exists(MFEM_INSTALL_DIR "/include/mfem/"))
   {
      mfemDir = MFEM_INSTALL_DIR "/include/mfem/";
   }
   else if (occa::io::exists(MFEM_SOURCE_DIR))
   {
      mfemDir = MFEM_SOURCE_DIR;
   }
   else
   {
      MFEM_ABORT("Cannot find OCCA kernels in MFEM_INSTALL_DIR or MFEM_SOURCE_DIR");
   }

   occa::io::addLibraryPath("mfem", mfemDir);
   occa::loadKernels("mfem");
#else
   MFEM_ABORT("the OCCA backends require MFEM built with MFEM_USE_OCCA=YES");
#endif
}

static void CeedDeviceSetup(const char* ceed_spec)
{
#ifdef MFEM_USE_CEED
   CeedInit(ceed_spec, &internal::ceed);
#endif
}

void Device::Setup(const int device)
{
   MFEM_VERIFY(ngpu == -1, "the mfem::Device is already configured!");

   ngpu = 0;
   dev = device;

#ifndef MFEM_USE_CUDA
   MFEM_VERIFY(!Allows(Backend::CUDA_MASK),
               "the CUDA backends require MFEM built with MFEM_USE_CUDA=YES");
#endif
#ifndef MFEM_USE_RAJA
   MFEM_VERIFY(!Allows(Backend::RAJA_MASK),
               "the RAJA backends require MFEM built with MFEM_USE_RAJA=YES");
#endif
#ifndef MFEM_USE_OPENMP
   MFEM_VERIFY(!Allows(Backend::OMP|Backend::RAJA_OMP),
               "the OpenMP and RAJA OpenMP backends require MFEM built with"
               " MFEM_USE_OPENMP=YES");
#endif
   if (Allows(Backend::CUDA)) { CudaDeviceSetup(dev, ngpu); }
   if (Allows(Backend::RAJA_CUDA)) { RajaDeviceSetup(dev, ngpu); }
   // The check for MFEM_USE_OCCA is in the function OccaDeviceSetup().
   if (Allows(Backend::OCCA_MASK)) { OccaDeviceSetup(dev); }
   if (Allows(Backend::CEED_CPU))
   {
      if (!ceed_option)
      {
         CeedDeviceSetup("/cpu/self/ref/blocked");
      }
      else
      {
         CeedDeviceSetup(ceed_option);
      }
   }
   if (Allows(Backend::CEED_CUDA))
   {
      if (!ceed_option)
      {
         CeedDeviceSetup("/gpu/cuda/ref");
      }
      else
      {
         CeedDeviceSetup(ceed_option);
      }
   }
}

} // mfem
