// Copyright (c) 2010-2020, Lawrence Livermore National Security, LLC. Produced
// at the Lawrence Livermore National Laboratory. All Rights reserved. See files
// LICENSE and NOTICE for details. LLNL-CODE-806117.
//
// This file is part of the MFEM library. For more information and source code
// availability visit https://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the BSD-3 license. We welcome feedback and contributions, see file
// CONTRIBUTING.md for details.

#include "../general/forall.hpp"
#include "bilininteg.hpp"
#include "gridfunc.hpp"
#include "libceed/mass.hpp"

using namespace std;

namespace mfem
{

// PA Mass Integrator

// PA Mass Assemble kernel

void MassIntegrator::SetupPA(const FiniteElementSpace &fes, const bool force)
{
   // Assuming the same element type
   fespace = &fes;
   Mesh *mesh = fes.GetMesh();
   if (mesh->GetNE() == 0) { return; }
   const FiniteElement &el = *fes.GetFE(0);
   ElementTransformation *T = mesh->GetElementTransformation(0);
   const IntegrationRule *ir = IntRule ? IntRule : &GetRule(el, el, *T);
#ifdef MFEM_USE_CEED
   if (DeviceCanUseCeed() && !force)
   {
      if (ceedDataPtr) { delete ceedDataPtr; }
      CeedData* ptr = new CeedData();
      ceedDataPtr = ptr;
      InitCeedCoeff(Q, ptr);
      return CeedPAMassAssemble(fes, *ir, *ptr);
   }
#else
   MFEM_CONTRACT_VAR(force);
#endif
   dim = mesh->Dimension();
   ne = fes.GetMesh()->GetNE();
   nq = ir->GetNPoints();
   geom = mesh->GetGeometricFactors(*ir, GeometricFactors::COORDINATES |
                                    GeometricFactors::JACOBIANS);
   maps = &el.GetDofToQuad(*ir, DofToQuad::TENSOR);
   dofs1D = maps->ndof;
   quad1D = maps->nqpt;
   pa_data.SetSize(ne*nq, Device::GetDeviceMemoryType());
   Vector coeff;
   if (Q == nullptr)
   {
      coeff.SetSize(1);
      coeff(0) = 1.0;
   }
   else if (ConstantCoefficient* cQ = dynamic_cast<ConstantCoefficient*>(Q))
   {
      coeff.SetSize(1);
      coeff(0) = cQ->constant;
   }
   else if (QuadratureFunctionCoefficient* cQ =
               dynamic_cast<QuadratureFunctionCoefficient*>(Q))
   {
      const QuadratureFunction &qFun = cQ->GetQuadFunction();
      MFEM_VERIFY(qFun.Size() == nq * ne,
                  "Incompatible QuadratureFunction dimension \n");

      MFEM_VERIFY(ir == &qFun.GetSpace()->GetElementIntRule(0),
                  "IntegrationRule used within integrator and in"
                  " QuadratureFunction appear to be different");
      qFun.Read();
      coeff.MakeRef(const_cast<QuadratureFunction &>(qFun),0);
   }
   else
   {
      coeff.SetSize(nq * ne);
      auto C = Reshape(coeff.HostWrite(), nq, ne);
      for (int e = 0; e < ne; ++e)
      {
         ElementTransformation& T = *fes.GetElementTransformation(e);
         for (int q = 0; q < nq; ++q)
         {
            C(q,e) = Q->Eval(T, ir->IntPoint(q));
         }
      }
   }
   if (dim==1) { MFEM_ABORT("Not supported yet... stay tuned!"); }
   if (dim==2)
   {
      const int NE = ne;
      const int NQ = nq;
      const bool const_c = coeff.Size() == 1;
      auto w = ir->GetWeights().Read();
      auto J = Reshape(geom->J.Read(), NQ,2,2,NE);
      auto C =
         const_c ? Reshape(coeff.Read(), 1,1) : Reshape(coeff.Read(), NQ,NE);
      auto v = Reshape(pa_data.Write(), NQ, NE);
      MFEM_FORALL(e, NE,
      {
         for (int q = 0; q < NQ; ++q)
         {
            const double J11 = J(q,0,0,e);
            const double J12 = J(q,1,0,e);
            const double J21 = J(q,0,1,e);
            const double J22 = J(q,1,1,e);
            const double detJ = (J11*J22)-(J21*J12);
            const double coeff = const_c ? C(0,0) : C(q,e);
            v(q,e) =  w[q] * coeff * detJ;
         }
      });
   }
   if (dim==3)
   {
      const int NE = ne;
      const int NQ = nq;
      const bool const_c = coeff.Size() == 1;
      auto W = ir->GetWeights().Read();
      auto J = Reshape(geom->J.Read(), NQ,3,3,NE);
      auto C =
         const_c ? Reshape(coeff.Read(), 1,1) : Reshape(coeff.Read(), NQ,NE);
      auto v = Reshape(pa_data.Write(), NQ,NE);
      MFEM_FORALL(e, NE,
      {
         for (int q = 0; q < NQ; ++q)
         {
            const double J11 = J(q,0,0,e), J12 = J(q,0,1,e), J13 = J(q,0,2,e);
            const double J21 = J(q,1,0,e), J22 = J(q,1,1,e), J23 = J(q,1,2,e);
            const double J31 = J(q,2,0,e), J32 = J(q,2,1,e), J33 = J(q,2,2,e);
            const double detJ = J11 * (J22 * J33 - J32 * J23) -
            /* */               J21 * (J12 * J33 - J32 * J13) +
            /* */               J31 * (J12 * J23 - J22 * J13);
            const double coeff = const_c ? C(0,0) : C(q,e);
            v(q,e) = W[q] * coeff * detJ;
         }
      });
   }
}

void MassIntegrator::AssemblePA(const FiniteElementSpace &fes)
{
   SetupPA(fes);
}


template<int T_D1D = 0, int T_Q1D = 0>
static void PAMassAssembleDiagonal2D(const int NE,
                                     const Array<double> &b,
                                     const Vector &d,
                                     Vector &y,
                                     const int d1d = 0,
                                     const int q1d = 0)
{
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;
   MFEM_VERIFY(D1D <= MAX_D1D, "");
   MFEM_VERIFY(Q1D <= MAX_Q1D, "");
   auto B = Reshape(b.Read(), Q1D, D1D);
   auto D = Reshape(d.Read(), Q1D, Q1D, NE);
   auto Y = Reshape(y.ReadWrite(), D1D, D1D, NE);
   MFEM_FORALL(e, NE,
   {
      const int D1D = T_D1D ? T_D1D : d1d;
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      constexpr int MD1 = T_D1D ? T_D1D : MAX_D1D;
      constexpr int MQ1 = T_Q1D ? T_Q1D : MAX_Q1D;
      double QD[MQ1][MD1];
      for (int qx = 0; qx < Q1D; ++qx)
      {
         for (int dy = 0; dy < D1D; ++dy)
         {
            QD[qx][dy] = 0.0;
            for (int qy = 0; qy < Q1D; ++qy)
            {
               QD[qx][dy] += B(qy, dy) * B(qy, dy) * D(qx, qy, e);
            }
         }
      }
      for (int dy = 0; dy < D1D; ++dy)
      {
         for (int dx = 0; dx < D1D; ++dx)
         {
            for (int qx = 0; qx < Q1D; ++qx)
            {
               Y(dx,dy,e) += B(qx, dx) * B(qx, dx) * QD[qx][dy];
            }
         }
      }
   });
}

template<int T_D1D = 0, int T_Q1D = 0, int T_NBZ = 0>
static void SmemPAMassAssembleDiagonal2D(const int NE,
                                         const Array<double> &b_,
                                         const Vector &d_,
                                         Vector &y_,
                                         const int d1d = 0,
                                         const int q1d = 0)
{
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;
   constexpr int NBZ = T_NBZ ? T_NBZ : 1;
   constexpr int MQ1 = T_Q1D ? T_Q1D : MAX_Q1D;
   constexpr int MD1 = T_D1D ? T_D1D : MAX_D1D;
   MFEM_VERIFY(D1D <= MD1, "");
   MFEM_VERIFY(Q1D <= MQ1, "");
   auto b = Reshape(b_.Read(), Q1D, D1D);
   auto D = Reshape(d_.Read(), Q1D, Q1D, NE);
   auto Y = Reshape(y_.ReadWrite(), D1D, D1D, NE);
   MFEM_FORALL_2D(e, NE, Q1D, Q1D, NBZ,
   {
      const int tidz = MFEM_THREAD_ID(z);
      const int D1D = T_D1D ? T_D1D : d1d;
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      constexpr int NBZ = T_NBZ ? T_NBZ : 1;
      constexpr int MQ1 = T_Q1D ? T_Q1D : MAX_Q1D;
      constexpr int MD1 = T_D1D ? T_D1D : MAX_D1D;
      MFEM_SHARED double B[MQ1][MD1];
      MFEM_SHARED double QDZ[NBZ][MQ1][MD1];
      double (*QD)[MD1] = (double (*)[MD1])(QDZ + tidz);
      if (tidz == 0)
      {
         MFEM_FOREACH_THREAD(d,y,D1D)
         {
            MFEM_FOREACH_THREAD(q,x,Q1D)
            {
               B[q][d] = b(q,d);
            }
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_FOREACH_THREAD(qx,x,Q1D)
      {
         MFEM_FOREACH_THREAD(dy,y,D1D)
         {
            QD[qx][dy] = 0.0;
            for (int qy = 0; qy < Q1D; ++qy)
            {
               QD[qx][dy] += B[qy][dy] * B[qy][dy] * D(qx, qy, e);
            }
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_FOREACH_THREAD(dy,y,D1D)
      {
         MFEM_FOREACH_THREAD(dx,x,D1D)
         {
            for (int qx = 0; qx < Q1D; ++qx)
            {
               // might need absolute values on next line
               Y(dx,dy,e) += B[qx][dx] * B[qx][dx] * QD[qx][dy];
            }
         }
      }
   });
}

template<int T_D1D = 0, int T_Q1D = 0>
static void PAMassAssembleDiagonal3D(const int NE,
                                     const Array<double> &b,
                                     const Vector &d,
                                     Vector &y,
                                     const int d1d = 0,
                                     const int q1d = 0)
{
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;
   MFEM_VERIFY(D1D <= MAX_D1D, "");
   MFEM_VERIFY(Q1D <= MAX_Q1D, "");
   auto B = Reshape(b.Read(), Q1D, D1D);
   auto D = Reshape(d.Read(), Q1D, Q1D, Q1D, NE);
   auto Y = Reshape(y.ReadWrite(), D1D, D1D, D1D, NE);
   MFEM_FORALL(e, NE,
   {
      const int D1D = T_D1D ? T_D1D : d1d;
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      constexpr int MD1 = T_D1D ? T_D1D : MAX_D1D;
      constexpr int MQ1 = T_Q1D ? T_Q1D : MAX_Q1D;
      double QQD[MQ1][MQ1][MD1];
      double QDD[MQ1][MD1][MD1];
      for (int qx = 0; qx < Q1D; ++qx)
      {
         for (int qy = 0; qy < Q1D; ++qy)
         {
            for (int dz = 0; dz < D1D; ++dz)
            {
               QQD[qx][qy][dz] = 0.0;
               for (int qz = 0; qz < Q1D; ++qz)
               {
                  QQD[qx][qy][dz] += B(qz, dz) * B(qz, dz) * D(qx, qy, qz, e);
               }
            }
         }
      }
      for (int qx = 0; qx < Q1D; ++qx)
      {
         for (int dz = 0; dz < D1D; ++dz)
         {
            for (int dy = 0; dy < D1D; ++dy)
            {
               QDD[qx][dy][dz] = 0.0;
               for (int qy = 0; qy < Q1D; ++qy)
               {
                  QDD[qx][dy][dz] += B(qy, dy) * B(qy, dy) * QQD[qx][qy][dz];
               }
            }
         }
      }
      for (int dz = 0; dz < D1D; ++dz)
      {
         for (int dy = 0; dy < D1D; ++dy)
         {
            for (int dx = 0; dx < D1D; ++dx)
            {
               double t = 0.0;
               for (int qx = 0; qx < Q1D; ++qx)
               {
                  t += B(qx, dx) * B(qx, dx) * QDD[qx][dy][dz];
               }
               Y(dx, dy, dz, e) += t;
            }
         }
      }
   });
}

template<int T_D1D = 0, int T_Q1D = 0>
static void SmemPAMassAssembleDiagonal3D(const int NE,
                                         const Array<double> &b_,
                                         const Vector &d_,
                                         Vector &y_,
                                         const int d1d = 0,
                                         const int q1d = 0)
{
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;
   constexpr int MQ1 = T_Q1D ? T_Q1D : MAX_Q1D;
   constexpr int MD1 = T_D1D ? T_D1D : MAX_D1D;
   MFEM_VERIFY(D1D <= MD1, "");
   MFEM_VERIFY(Q1D <= MQ1, "");
   auto b = Reshape(b_.Read(), Q1D, D1D);
   auto D = Reshape(d_.Read(), Q1D, Q1D, Q1D, NE);
   auto Y = Reshape(y_.ReadWrite(), D1D, D1D, D1D, NE);
   MFEM_FORALL_3D(e, NE, Q1D, Q1D, Q1D,
   {
      const int tidz = MFEM_THREAD_ID(z);
      const int D1D = T_D1D ? T_D1D : d1d;
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      constexpr int MD1 = T_D1D ? T_D1D : MAX_D1D;
      constexpr int MQ1 = T_Q1D ? T_Q1D : MAX_Q1D;
      MFEM_SHARED double B[MQ1][MD1];
      MFEM_SHARED double QQD[MQ1][MQ1][MD1];
      MFEM_SHARED double QDD[MQ1][MD1][MD1];
      if (tidz == 0)
      {
         MFEM_FOREACH_THREAD(d,y,D1D)
         {
            MFEM_FOREACH_THREAD(q,x,Q1D)
            {
               B[q][d] = b(q,d);
            }
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_FOREACH_THREAD(qx,x,Q1D)
      {
         MFEM_FOREACH_THREAD(qy,y,Q1D)
         {
            MFEM_FOREACH_THREAD(dz,z,D1D)
            {
               QQD[qx][qy][dz] = 0.0;
               for (int qz = 0; qz < Q1D; ++qz)
               {
                  QQD[qx][qy][dz] += B[qz][dz] * B[qz][dz] * D(qx, qy, qz, e);
               }
            }
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_FOREACH_THREAD(qx,x,Q1D)
      {
         MFEM_FOREACH_THREAD(dz,z,D1D)
         {
            MFEM_FOREACH_THREAD(dy,y,D1D)
            {
               QDD[qx][dy][dz] = 0.0;
               for (int qy = 0; qy < Q1D; ++qy)
               {
                  QDD[qx][dy][dz] += B[qy][dy] * B[qy][dy] * QQD[qx][qy][dz];
               }
            }
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_FOREACH_THREAD(dz,z,D1D)
      {
         MFEM_FOREACH_THREAD(dy,y,D1D)
         {
            MFEM_FOREACH_THREAD(dx,x,D1D)
            {
               double t = 0.0;
               for (int qx = 0; qx < Q1D; ++qx)
               {
                  t += B[qx][dx] * B[qx][dx] * QDD[qx][dy][dz];
               }
               Y(dx, dy, dz, e) += t;
            }
         }
      }
   });
}

static void PAMassAssembleDiagonal(const int dim, const int D1D,
                                   const int Q1D, const int NE,
                                   const Array<double> &B,
                                   const Vector &D,
                                   Vector &Y)
{
   if (dim == 2)
   {
      switch ((D1D << 4 ) | Q1D)
      {
         case 0x22: return SmemPAMassAssembleDiagonal2D<2,2,16>(NE,B,D,Y);
         case 0x33: return SmemPAMassAssembleDiagonal2D<3,3,16>(NE,B,D,Y);
         case 0x44: return SmemPAMassAssembleDiagonal2D<4,4,8>(NE,B,D,Y);
         case 0x55: return SmemPAMassAssembleDiagonal2D<5,5,8>(NE,B,D,Y);
         case 0x66: return SmemPAMassAssembleDiagonal2D<6,6,4>(NE,B,D,Y);
         case 0x77: return SmemPAMassAssembleDiagonal2D<7,7,4>(NE,B,D,Y);
         case 0x88: return SmemPAMassAssembleDiagonal2D<8,8,2>(NE,B,D,Y);
         case 0x99: return SmemPAMassAssembleDiagonal2D<9,9,2>(NE,B,D,Y);
         default:   return PAMassAssembleDiagonal2D(NE,B,D,Y,D1D,Q1D);
      }
   }
   else if (dim == 3)
   {
      switch ((D1D << 4 ) | Q1D)
      {
         case 0x23: return SmemPAMassAssembleDiagonal3D<2,3>(NE,B,D,Y);
         case 0x34: return SmemPAMassAssembleDiagonal3D<3,4>(NE,B,D,Y);
         case 0x45: return SmemPAMassAssembleDiagonal3D<4,5>(NE,B,D,Y);
         case 0x56: return SmemPAMassAssembleDiagonal3D<5,6>(NE,B,D,Y);
         case 0x67: return SmemPAMassAssembleDiagonal3D<6,7>(NE,B,D,Y);
         case 0x78: return SmemPAMassAssembleDiagonal3D<7,8>(NE,B,D,Y);
         case 0x89: return SmemPAMassAssembleDiagonal3D<8,9>(NE,B,D,Y);
         default:   return PAMassAssembleDiagonal3D(NE,B,D,Y,D1D,Q1D);
      }
   }
   MFEM_ABORT("Unknown kernel.");
}

void MassIntegrator::AssembleDiagonalPA(Vector &diag)
{
   if (pa_data.Size()==0) { SetupPA(*fespace, true); }
   PAMassAssembleDiagonal(dim, dofs1D, quad1D, ne, maps->B, pa_data, diag);
}


#ifdef MFEM_USE_OCCA
// OCCA PA Mass Apply 2D kernel
static void OccaPAMassApply2D(const int D1D,
                              const int Q1D,
                              const int NE,
                              const Array<double> &B,
                              const Array<double> &Bt,
                              const Vector &D,
                              const Vector &X,
                              Vector &Y)
{
   occa::properties props;
   props["defines/D1D"] = D1D;
   props["defines/Q1D"] = Q1D;
   const occa::memory o_B = OccaMemoryRead(B.GetMemory(), B.Size());
   const occa::memory o_Bt = OccaMemoryRead(Bt.GetMemory(), Bt.Size());
   const occa::memory o_D = OccaMemoryRead(D.GetMemory(), D.Size());
   const occa::memory o_X = OccaMemoryRead(X.GetMemory(), X.Size());
   occa::memory o_Y = OccaMemoryReadWrite(Y.GetMemory(), Y.Size());
   const occa_id_t id = std::make_pair(D1D,Q1D);
   if (!Device::Allows(Backend::OCCA_CUDA))
   {
      static occa_kernel_t OccaMassApply2D_cpu;
      if (OccaMassApply2D_cpu.find(id) == OccaMassApply2D_cpu.end())
      {
         const occa::kernel MassApply2D_CPU =
            mfem::OccaDev().buildKernel("occa://mfem/fem/occa.okl",
                                        "MassApply2D_CPU", props);
         OccaMassApply2D_cpu.emplace(id, MassApply2D_CPU);
      }
      OccaMassApply2D_cpu.at(id)(NE, o_B, o_Bt, o_D, o_X, o_Y);
   }
   else
   {
      static occa_kernel_t OccaMassApply2D_gpu;
      if (OccaMassApply2D_gpu.find(id) == OccaMassApply2D_gpu.end())
      {
         const occa::kernel MassApply2D_GPU =
            mfem::OccaDev().buildKernel("occa://mfem/fem/occa.okl",
                                        "MassApply2D_GPU", props);
         OccaMassApply2D_gpu.emplace(id, MassApply2D_GPU);
      }
      OccaMassApply2D_gpu.at(id)(NE, o_B, o_Bt, o_D, o_X, o_Y);
   }
}

// OCCA PA Mass Apply 3D kernel
static void OccaPAMassApply3D(const int D1D,
                              const int Q1D,
                              const int NE,
                              const Array<double> &B,
                              const Array<double> &Bt,
                              const Vector &D,
                              const Vector &X,
                              Vector &Y)
{
   occa::properties props;
   props["defines/D1D"] = D1D;
   props["defines/Q1D"] = Q1D;
   const occa::memory o_B = OccaMemoryRead(B.GetMemory(), B.Size());
   const occa::memory o_Bt = OccaMemoryRead(Bt.GetMemory(), Bt.Size());
   const occa::memory o_D = OccaMemoryRead(D.GetMemory(), D.Size());
   const occa::memory o_X = OccaMemoryRead(X.GetMemory(), X.Size());
   occa::memory o_Y = OccaMemoryReadWrite(Y.GetMemory(), Y.Size());
   const occa_id_t id = std::make_pair(D1D,Q1D);
   if (!Device::Allows(Backend::OCCA_CUDA))
   {
      static occa_kernel_t OccaMassApply3D_cpu;
      if (OccaMassApply3D_cpu.find(id) == OccaMassApply3D_cpu.end())
      {
         const occa::kernel MassApply3D_CPU =
            mfem::OccaDev().buildKernel("occa://mfem/fem/occa.okl",
                                        "MassApply3D_CPU", props);
         OccaMassApply3D_cpu.emplace(id, MassApply3D_CPU);
      }
      OccaMassApply3D_cpu.at(id)(NE, o_B, o_Bt, o_D, o_X, o_Y);
   }
   else
   {
      static occa_kernel_t OccaMassApply3D_gpu;
      if (OccaMassApply3D_gpu.find(id) == OccaMassApply3D_gpu.end())
      {
         const occa::kernel MassApply3D_GPU =
            mfem::OccaDev().buildKernel("occa://mfem/fem/occa.okl",
                                        "MassApply3D_GPU", props);
         OccaMassApply3D_gpu.emplace(id, MassApply3D_GPU);
      }
      OccaMassApply3D_gpu.at(id)(NE, o_B, o_Bt, o_D, o_X, o_Y);
   }
}
#endif // MFEM_USE_OCCA

template<int T_D1D = 0, int T_Q1D = 0>
static void PAMassApply2D(const int NE,
                          const Array<double> &b_,
                          const Array<double> &bt_,
                          const Vector &d_,
                          const Vector &x_,
                          Vector &y_,
                          const int d1d = 0,
                          const int q1d = 0)
{
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;
   MFEM_VERIFY(D1D <= MAX_D1D, "");
   MFEM_VERIFY(Q1D <= MAX_Q1D, "");
   auto B = Reshape(b_.Read(), Q1D, D1D);
   auto Bt = Reshape(bt_.Read(), D1D, Q1D);
   auto D = Reshape(d_.Read(), Q1D, Q1D, NE);
   auto X = Reshape(x_.Read(), D1D, D1D, NE);
   auto Y = Reshape(y_.ReadWrite(), D1D, D1D, NE);
   MFEM_FORALL(e, NE,
   {
      const int D1D = T_D1D ? T_D1D : d1d; // nvcc workaround
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      // the following variables are evaluated at compile time
      constexpr int max_D1D = T_D1D ? T_D1D : MAX_D1D;
      constexpr int max_Q1D = T_Q1D ? T_Q1D : MAX_Q1D;
      double sol_xy[max_Q1D][max_Q1D];
      for (int qy = 0; qy < Q1D; ++qy)
      {
         for (int qx = 0; qx < Q1D; ++qx)
         {
            sol_xy[qy][qx] = 0.0;
         }
      }
      for (int dy = 0; dy < D1D; ++dy)
      {
         double sol_x[max_Q1D];
         for (int qy = 0; qy < Q1D; ++qy)
         {
            sol_x[qy] = 0.0;
         }
         for (int dx = 0; dx < D1D; ++dx)
         {
            const double s = X(dx,dy,e);
            for (int qx = 0; qx < Q1D; ++qx)
            {
               sol_x[qx] += B(qx,dx)* s;
            }
         }
         for (int qy = 0; qy < Q1D; ++qy)
         {
            const double d2q = B(qy,dy);
            for (int qx = 0; qx < Q1D; ++qx)
            {
               sol_xy[qy][qx] += d2q * sol_x[qx];
            }
         }
      }
      for (int qy = 0; qy < Q1D; ++qy)
      {
         for (int qx = 0; qx < Q1D; ++qx)
         {
            sol_xy[qy][qx] *= D(qx,qy,e);
         }
      }
      for (int qy = 0; qy < Q1D; ++qy)
      {
         double sol_x[max_D1D];
         for (int dx = 0; dx < D1D; ++dx)
         {
            sol_x[dx] = 0.0;
         }
         for (int qx = 0; qx < Q1D; ++qx)
         {
            const double s = sol_xy[qy][qx];
            for (int dx = 0; dx < D1D; ++dx)
            {
               sol_x[dx] += Bt(dx,qx) * s;
            }
         }
         for (int dy = 0; dy < D1D; ++dy)
         {
            const double q2d = Bt(dy,qy);
            for (int dx = 0; dx < D1D; ++dx)
            {
               Y(dx,dy,e) += q2d * sol_x[dx];
            }
         }
      }
   });
}

template<int T_D1D = 0, int T_Q1D = 0, int T_NBZ = 0>
static void SmemPAMassApply2D(const int NE,
                              const Array<double> &b_,
                              const Array<double> &bt_,
                              const Vector &d_,
                              const Vector &x_,
                              Vector &y_,
                              const int d1d = 0,
                              const int q1d = 0)
{
   MFEM_CONTRACT_VAR(bt_);
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;
   constexpr int NBZ = T_NBZ ? T_NBZ : 1;
   constexpr int MQ1 = T_Q1D ? T_Q1D : MAX_Q1D;
   constexpr int MD1 = T_D1D ? T_D1D : MAX_D1D;
   MFEM_VERIFY(D1D <= MD1, "");
   MFEM_VERIFY(Q1D <= MQ1, "");
   auto b = Reshape(b_.Read(), Q1D, D1D);
   auto D = Reshape(d_.Read(), Q1D, Q1D, NE);
   auto x = Reshape(x_.Read(), D1D, D1D, NE);
   auto Y = Reshape(y_.ReadWrite(), D1D, D1D, NE);
   MFEM_FORALL_2D(e, NE, Q1D, Q1D, NBZ,
   {
      const int tidz = MFEM_THREAD_ID(z);
      const int D1D = T_D1D ? T_D1D : d1d;
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      constexpr int NBZ = T_NBZ ? T_NBZ : 1;
      constexpr int MQ1 = T_Q1D ? T_Q1D : MAX_Q1D;
      constexpr int MD1 = T_D1D ? T_D1D : MAX_D1D;
      constexpr int MDQ = (MQ1 > MD1) ? MQ1 : MD1;
      MFEM_SHARED double BBt[MQ1*MD1];
      double (*B)[MD1] = (double (*)[MD1]) BBt;
      double (*Bt)[MQ1] = (double (*)[MQ1]) BBt;
      MFEM_SHARED double sm0[NBZ][MDQ*MDQ];
      MFEM_SHARED double sm1[NBZ][MDQ*MDQ];
      double (*X)[MD1] = (double (*)[MD1]) (sm0 + tidz);
      double (*DQ)[MQ1] = (double (*)[MQ1]) (sm1 + tidz);
      double (*QQ)[MQ1] = (double (*)[MQ1]) (sm0 + tidz);
      double (*QD)[MD1] = (double (*)[MD1]) (sm1 + tidz);
      MFEM_FOREACH_THREAD(dy,y,D1D)
      {
         MFEM_FOREACH_THREAD(dx,x,D1D)
         {
            X[dy][dx] = x(dx,dy,e);
         }
      }
      if (tidz == 0)
      {
         MFEM_FOREACH_THREAD(dy,y,D1D)
         {
            MFEM_FOREACH_THREAD(q,x,Q1D)
            {
               B[q][dy] = b(q,dy);
            }
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_FOREACH_THREAD(dy,y,D1D)
      {
         MFEM_FOREACH_THREAD(qx,x,Q1D)
         {
            double dq = 0.0;
            for (int dx = 0; dx < D1D; ++dx)
            {
               dq += X[dy][dx] * B[qx][dx];
            }
            DQ[dy][qx] = dq;
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_FOREACH_THREAD(qy,y,Q1D)
      {
         MFEM_FOREACH_THREAD(qx,x,Q1D)
         {
            double qq = 0.0;
            for (int dy = 0; dy < D1D; ++dy)
            {
               qq += DQ[dy][qx] * B[qy][dy];
            }
            QQ[qy][qx] = qq * D(qx, qy, e);
         }
      }
      MFEM_SYNC_THREAD;
      if (tidz == 0)
      {
         MFEM_FOREACH_THREAD(dy,y,D1D)
         {
            MFEM_FOREACH_THREAD(q,x,Q1D)
            {
               Bt[dy][q] = b(q,dy);
            }
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_FOREACH_THREAD(qy,y,Q1D)
      {
         MFEM_FOREACH_THREAD(dx,x,D1D)
         {
            double dq = 0.0;
            for (int qx = 0; qx < Q1D; ++qx)
            {
               dq += QQ[qy][qx] * Bt[dx][qx];
            }
            QD[qy][dx] = dq;
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_FOREACH_THREAD(dy,y,D1D)
      {
         MFEM_FOREACH_THREAD(dx,x,D1D)
         {
            double dd = 0.0;
            for (int qy = 0; qy < Q1D; ++qy)
            {
               dd += (QD[qy][dx] * Bt[dy][qy]);
            }
            Y(dx, dy, e) += dd;
         }
      }
   });
}

template<int T_D1D = 0, int T_Q1D = 0>
static void PAMassApply3D(const int NE,
                          const Array<double> &b_,
                          const Array<double> &bt_,
                          const Vector &d_,
                          const Vector &x_,
                          Vector &y_,
                          const int d1d = 0,
                          const int q1d = 0)
{
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;
   MFEM_VERIFY(D1D <= MAX_D1D, "");
   MFEM_VERIFY(Q1D <= MAX_Q1D, "");
   auto B = Reshape(b_.Read(), Q1D, D1D);
   auto Bt = Reshape(bt_.Read(), D1D, Q1D);
   auto D = Reshape(d_.Read(), Q1D, Q1D, Q1D, NE);
   auto X = Reshape(x_.Read(), D1D, D1D, D1D, NE);
   auto Y = Reshape(y_.ReadWrite(), D1D, D1D, D1D, NE);
   MFEM_FORALL(e, NE,
   {
      const int D1D = T_D1D ? T_D1D : d1d;
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      constexpr int max_D1D = T_D1D ? T_D1D : MAX_D1D;
      constexpr int max_Q1D = T_Q1D ? T_Q1D : MAX_Q1D;
      double sol_xyz[max_Q1D][max_Q1D][max_Q1D];
      for (int qz = 0; qz < Q1D; ++qz)
      {
         for (int qy = 0; qy < Q1D; ++qy)
         {
            for (int qx = 0; qx < Q1D; ++qx)
            {
               sol_xyz[qz][qy][qx] = 0.0;
            }
         }
      }
      for (int dz = 0; dz < D1D; ++dz)
      {
         double sol_xy[max_Q1D][max_Q1D];
         for (int qy = 0; qy < Q1D; ++qy)
         {
            for (int qx = 0; qx < Q1D; ++qx)
            {
               sol_xy[qy][qx] = 0.0;
            }
         }
         for (int dy = 0; dy < D1D; ++dy)
         {
            double sol_x[max_Q1D];
            for (int qx = 0; qx < Q1D; ++qx)
            {
               sol_x[qx] = 0;
            }
            for (int dx = 0; dx < D1D; ++dx)
            {
               const double s = X(dx,dy,dz,e);
               for (int qx = 0; qx < Q1D; ++qx)
               {
                  sol_x[qx] += B(qx,dx) * s;
               }
            }
            for (int qy = 0; qy < Q1D; ++qy)
            {
               const double wy = B(qy,dy);
               for (int qx = 0; qx < Q1D; ++qx)
               {
                  sol_xy[qy][qx] += wy * sol_x[qx];
               }
            }
         }
         for (int qz = 0; qz < Q1D; ++qz)
         {
            const double wz = B(qz,dz);
            for (int qy = 0; qy < Q1D; ++qy)
            {
               for (int qx = 0; qx < Q1D; ++qx)
               {
                  sol_xyz[qz][qy][qx] += wz * sol_xy[qy][qx];
               }
            }
         }
      }
      for (int qz = 0; qz < Q1D; ++qz)
      {
         for (int qy = 0; qy < Q1D; ++qy)
         {
            for (int qx = 0; qx < Q1D; ++qx)
            {
               sol_xyz[qz][qy][qx] *= D(qx,qy,qz,e);
            }
         }
      }
      for (int qz = 0; qz < Q1D; ++qz)
      {
         double sol_xy[max_D1D][max_D1D];
         for (int dy = 0; dy < D1D; ++dy)
         {
            for (int dx = 0; dx < D1D; ++dx)
            {
               sol_xy[dy][dx] = 0;
            }
         }
         for (int qy = 0; qy < Q1D; ++qy)
         {
            double sol_x[max_D1D];
            for (int dx = 0; dx < D1D; ++dx)
            {
               sol_x[dx] = 0;
            }
            for (int qx = 0; qx < Q1D; ++qx)
            {
               const double s = sol_xyz[qz][qy][qx];
               for (int dx = 0; dx < D1D; ++dx)
               {
                  sol_x[dx] += Bt(dx,qx) * s;
               }
            }
            for (int dy = 0; dy < D1D; ++dy)
            {
               const double wy = Bt(dy,qy);
               for (int dx = 0; dx < D1D; ++dx)
               {
                  sol_xy[dy][dx] += wy * sol_x[dx];
               }
            }
         }
         for (int dz = 0; dz < D1D; ++dz)
         {
            const double wz = Bt(dz,qz);
            for (int dy = 0; dy < D1D; ++dy)
            {
               for (int dx = 0; dx < D1D; ++dx)
               {
                  Y(dx,dy,dz,e) += wz * sol_xy[dy][dx];
               }
            }
         }
      }
   });
}

template<int T_D1D = 0, int T_Q1D = 0>
static void SmemPAMassApply3D(const int NE,
                              const Array<double> &b_,
                              const Array<double> &bt_,
                              const Vector &d_,
                              const Vector &x_,
                              Vector &y_,
                              const int d1d = 0,
                              const int q1d = 0)
{
   MFEM_CONTRACT_VAR(bt_);
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;
   constexpr int M1Q = T_Q1D ? T_Q1D : MAX_Q1D;
   constexpr int M1D = T_D1D ? T_D1D : MAX_D1D;
   MFEM_VERIFY(D1D <= M1D, "");
   MFEM_VERIFY(Q1D <= M1Q, "");
   auto b = Reshape(b_.Read(), Q1D, D1D);
   auto d = Reshape(d_.Read(), Q1D, Q1D, Q1D, NE);
   auto x = Reshape(x_.Read(), D1D, D1D, D1D, NE);
   auto y = Reshape(y_.ReadWrite(), D1D, D1D, D1D, NE);
   MFEM_FORALL_3D(e, NE, Q1D, Q1D, 1,
   {
      const int D1D = T_D1D ? T_D1D : d1d;
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      constexpr int MQ1 = T_Q1D ? T_Q1D : MAX_Q1D;
      constexpr int MD1 = T_D1D ? T_D1D : MAX_D1D;
      constexpr int MDQ = (MQ1 > MD1) ? MQ1 : MD1;
      MFEM_SHARED double sDQ[MQ1*MD1];
      double (*B)[MD1] = (double (*)[MD1]) sDQ;
      double (*Bt)[MQ1] = (double (*)[MQ1]) sDQ;
      MFEM_SHARED double sm0[MDQ*MDQ*MDQ];
      MFEM_SHARED double sm1[MDQ*MDQ*MDQ];
      double (*X)[MD1][MD1]   = (double (*)[MD1][MD1]) sm0;
      double (*DDQ)[MD1][MQ1] = (double (*)[MD1][MQ1]) sm1;
      double (*DQQ)[MQ1][MQ1] = (double (*)[MQ1][MQ1]) sm0;
      double (*QQQ)[MQ1][MQ1] = (double (*)[MQ1][MQ1]) sm1;
      double (*QQD)[MQ1][MD1] = (double (*)[MQ1][MD1]) sm0;
      double (*QDD)[MD1][MD1] = (double (*)[MD1][MD1]) sm1;
      MFEM_FOREACH_THREAD(dy,y,D1D)
      {
         MFEM_FOREACH_THREAD(dx,x,D1D)
         {
            MFEM_UNROLL(MD1)
            for (int dz = 0; dz < D1D; ++dz)
            {
               X[dz][dy][dx] = x(dx,dy,dz,e);
            }
         }
         MFEM_FOREACH_THREAD(dx,x,Q1D)
         {
            B[dx][dy] = b(dx,dy);
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_FOREACH_THREAD(dy,y,D1D)
      {
         MFEM_FOREACH_THREAD(qx,x,Q1D)
         {
            double u[D1D];
            MFEM_UNROLL(MD1)
            for (int dz = 0; dz < D1D; dz++)
            {
               u[dz] = 0;
            }
            MFEM_UNROLL(MD1)
            for (int dx = 0; dx < D1D; ++dx)
            {
               MFEM_UNROLL(MD1)
               for (int dz = 0; dz < D1D; ++dz)
               {
                  u[dz] += X[dz][dy][dx] * B[qx][dx];
               }
            }
            MFEM_UNROLL(MD1)
            for (int dz = 0; dz < D1D; ++dz)
            {
               DDQ[dz][dy][qx] = u[dz];
            }
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_FOREACH_THREAD(qy,y,Q1D)
      {
         MFEM_FOREACH_THREAD(qx,x,Q1D)
         {
            double u[D1D];
            MFEM_UNROLL(MD1)
            for (int dz = 0; dz < D1D; dz++)
            {
               u[dz] = 0;
            }
            MFEM_UNROLL(MD1)
            for (int dy = 0; dy < D1D; ++dy)
            {
               MFEM_UNROLL(MD1)
               for (int dz = 0; dz < D1D; dz++)
               {
                  u[dz] += DDQ[dz][dy][qx] * B[qy][dy];
               }
            }
            MFEM_UNROLL(MD1)
            for (int dz = 0; dz < D1D; dz++)
            {
               DQQ[dz][qy][qx] = u[dz];
            }
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_FOREACH_THREAD(qy,y,Q1D)
      {
         MFEM_FOREACH_THREAD(qx,x,Q1D)
         {
            double u[Q1D];
            MFEM_UNROLL(MQ1)
            for (int qz = 0; qz < Q1D; qz++)
            {
               u[qz] = 0;
            }
            MFEM_UNROLL(MD1)
            for (int dz = 0; dz < D1D; ++dz)
            {
               MFEM_UNROLL(MQ1)
               for (int qz = 0; qz < Q1D; qz++)
               {
                  u[qz] += DQQ[dz][qy][qx] * B[qz][dz];
               }
            }
            MFEM_UNROLL(MQ1)
            for (int qz = 0; qz < Q1D; qz++)
            {
               QQQ[qz][qy][qx] = u[qz] * d(qx,qy,qz,e);
            }
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_FOREACH_THREAD(d,y,D1D)
      {
         MFEM_FOREACH_THREAD(q,x,Q1D)
         {
            Bt[d][q] = b(q,d);
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_FOREACH_THREAD(qy,y,Q1D)
      {
         MFEM_FOREACH_THREAD(dx,x,D1D)
         {
            double u[Q1D];
            MFEM_UNROLL(MQ1)
            for (int qz = 0; qz < Q1D; ++qz)
            {
               u[qz] = 0;
            }
            MFEM_UNROLL(MQ1)
            for (int qx = 0; qx < Q1D; ++qx)
            {
               MFEM_UNROLL(MQ1)
               for (int qz = 0; qz < Q1D; ++qz)
               {
                  u[qz] += QQQ[qz][qy][qx] * Bt[dx][qx];
               }
            }
            MFEM_UNROLL(MQ1)
            for (int qz = 0; qz < Q1D; ++qz)
            {
               QQD[qz][qy][dx] = u[qz];
            }
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_FOREACH_THREAD(dy,y,D1D)
      {
         MFEM_FOREACH_THREAD(dx,x,D1D)
         {
            double u[Q1D];
            MFEM_UNROLL(MQ1)
            for (int qz = 0; qz < Q1D; ++qz)
            {
               u[qz] = 0;
            }
            MFEM_UNROLL(MQ1)
            for (int qy = 0; qy < Q1D; ++qy)
            {
               MFEM_UNROLL(MQ1)
               for (int qz = 0; qz < Q1D; ++qz)
               {
                  u[qz] += QQD[qz][qy][dx] * Bt[dy][qy];
               }
            }
            MFEM_UNROLL(MQ1)
            for (int qz = 0; qz < Q1D; ++qz)
            {
               QDD[qz][dy][dx] = u[qz];
            }
         }
      }
      MFEM_SYNC_THREAD;
      MFEM_FOREACH_THREAD(dy,y,D1D)
      {
         MFEM_FOREACH_THREAD(dx,x,D1D)
         {
            double u[D1D];
            MFEM_UNROLL(MD1)
            for (int dz = 0; dz < D1D; ++dz)
            {
               u[dz] = 0;
            }
            MFEM_UNROLL(MQ1)
            for (int qz = 0; qz < Q1D; ++qz)
            {
               MFEM_UNROLL(MD1)
               for (int dz = 0; dz < D1D; ++dz)
               {
                  u[dz] += QDD[qz][dy][dx] * Bt[dz][qz];
               }
            }
            MFEM_UNROLL(MD1)
            for (int dz = 0; dz < D1D; ++dz)
            {
               y(dx,dy,dz,e) += u[dz];
            }
         }
      }
   });
}

  /*
  using policy1_HOST = RAJA::LPolicy<RAJA::HOST, RAJA::loop_exec>;
#ifdef RAJA_ENABLE_CUDA
  using policy1x_DEVICE = RAJA::LPolicy<RAJA::DEVICE, RAJA::cuda_block_x_direct>;
  using policy1y_DEVICE = RAJA::LPolicy<RAJA::DEVICE, RAJA::cuda_block_y_direct>;
#else
  using policy1_DEVICE = RAJA::LPolicy<RAJA::DEVICE, RAJA::loop_exec>;
#endif
  using outer0 = camp::list<policy1_HOST, policy1x_DEVICE>;
  using outer1 = camp::list<policy1_HOST, policy1y_DEVICE>;

  using policy2_HOST = RAJA::LPolicy<RAJA::HOST, RAJA::loop_exec>;
#ifdef RAJA_ENABLE_CUDA
  using policy2x_DEVICE = RAJA::LPolicy<RAJA::DEVICE, RAJA::cuda_thread_x_loop>;
  using policy2y_DEVICE = RAJA::LPolicy<RAJA::DEVICE, RAJA::cuda_thread_y_loop>;
  using policy2z_DEVICE = RAJA::LPolicy<RAJA::DEVICE, RAJA::cuda_thread_z_loop>;
#else
  using policy2_DEVICE = RAJA::LPolicy<RAJA::DEVICE, RAJA::loop_exec>;
#endif
  */
using team0 = RAJA::LoopPolicy<RAJA::loop_exec,
                                RAJA::cuda_block_x_direct
                                >;

using thread1 = RAJA::LoopPolicy<RAJA::loop_exec,
                                  RAJA::cuda_thread_y_loop>;

using thread0 = RAJA::LoopPolicy<RAJA::loop_exec,
                                  RAJA::cuda_thread_x_loop>;

using thread01 = RAJA::LoopPolicy<RAJA::loop_exec,
                                  RAJA::cuda_thread_xyz_direct<2>>;

#define s_B_(x, y) s_B[x + Q1D*y]
#define s_Bt_(x, y) s_Bt[x + D1D*y]
#define s_xy_(x, y) s_xy[x + M1D*y]
  //#define X_(dx, dy, dz, e) X[dx + D1D*dy + D1D*D1D*dz + D1D*D1D*D1D*e]
  //#define Y_(dx, dy, dz, e) Y[dx + D1D*dy + D1D*D1D*dz + D1D*D1D*D1D*e]
  //#define D_(qx, qy, qz, e) D[qx + Q1D*qy + Q1D*Q1D*qz + Q1D*Q1D*Q1D*e]

template<int T_D1D = 0, int T_Q1D = 0>
static void RajaSmemPAMassApply3D(const int NE,
                              const Array<double> &b_,
                              const Array<double> &bt_,
                              const Vector &d_,
                              const Vector &x_,
                              Vector &y_,
                              const int d1d = 0,
                              const int q1d = 0)
{
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;
   //constexpr int M1Q = T_Q1D ? T_Q1D : MAX_Q1D;
   //constexpr int M1D = T_D1D ? T_D1D : MAX_D1D;

   //MFEM_VERIFY(D1D <= M1D, "");
   //MFEM_VERIFY(Q1D <= M1Q, "");
   const double *B = b_.Read();
   const double *Bt = bt_.Read();

   auto d = Reshape(d_.Read(), Q1D, Q1D, Q1D, NE);
   auto x = Reshape(x_.Read(), D1D, D1D, D1D, NE);
   auto y = Reshape(y_.ReadWrite(), D1D, D1D, D1D, NE);

   //Run time option
   RAJA::ExecPlace select_cpu_or_gpu;
   if(Device::Allows(Backend::CUDA_MASK))
   {
     select_cpu_or_gpu = RAJA::ExecPlace::DEVICE;
   }else
   {
     select_cpu_or_gpu = RAJA::ExecPlace::HOST;
   }

   using launch_policy =
     RAJA::LaunchPolicy<RAJA::seq_launch_t, RAJA::cuda_launch_t<true>>;

#if 1
   //using ThreadExclusive_t = RAJA::ThreadExclusive<Q1D,Q1D,1>;

   constexpr int M1D = D1D > Q1D ? D1D : Q1D;
   RAJA::RangeSegment TBounds(0, M1D);
   RAJA::launch<launch_policy>(select_cpu_or_gpu,
                               RAJA::Resources(RAJA::Teams(NE),RAJA::Threads(M1D, M1D)),
   [=] RAJA_HOST_DEVICE (RAJA::LaunchContext ctx) {

      const int D1D = T_D1D ? T_D1D : d1d;
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      constexpr int DQ1D = D1D*Q1D;
      constexpr int M1D = D1D > Q1D ? D1D : Q1D;
      constexpr int M2D = M1D*M1D;

      //Element Index
      RAJA::loop<team0>(ctx, RAJA::RangeSegment(0, NE), [&](int e) {

      //Basis functions sampled at quadrature points in 1D
      TEAM_SHARED double s_B[DQ1D];
      TEAM_SHARED double s_Bt[DQ1D];

      //Space for solution in the xy-plane
      TEAM_SHARED double s_xy[M2D];

      //Thread private memory
      //ThreadExclusive_t::ExclusiveMem
      RAJA::PrivateMemoryImpl<double, Q1D, M1D, M1D,1> r_z;
      RAJA::PrivateMemoryImpl<double, Q1D, M1D, M1D,1> r_z2;
      //double r_z[Q1D];
      //double r_z2[D1D];

      RAJA::RangeSegment TBounds(0, M1D);
      RAJA::loop<thread1>(ctx, TBounds, [&](int y) {
          RAJA::loop<thread0>(ctx, TBounds, [&](int x) {

        const int id = (y * M1D) + x;
        // Fetch Q <--> D maps
        if (id < DQ1D) {
          s_B[id]  = B[id];
          s_Bt[id]  = Bt[id];
        }
        // Initialize our Z axis
        for (int qz = 0; qz < Q1D; ++qz) {
          r_z(qz,x,y) = 0;
        }
        for (int dz = 0; dz < D1D; ++dz) {
          r_z2(dz,x,y) = 0;
        }
      });
    });

     RAJA::loop<thread1>(ctx, TBounds, [&](int dy) {
         RAJA::loop<thread0>(ctx, TBounds, [&](int dx) {
     
        if ((dx < D1D) && (dy < D1D)) {
          for (int dz = 0; dz < D1D; ++dz) {
            const double s = x(dx, dy, dz, e);
            // Calculate D -> Q in the Z axis
            for (int qz = 0; qz < Q1D; ++qz) {
              r_z(qz,dx,dy) += s * s_B_(qz, dz);
            }
          }
        }

      });
    });


     // For each xy plane
    for (int qz = 0; qz < Q1D; ++qz) {

      // Fill xy plane at given z position
      RAJA::loop<thread1>(ctx, TBounds, [&](int dy) { 
         RAJA::loop<thread0>(ctx, TBounds, [&](int dx) {
          if ((dx < D1D) && (dy < D1D)) {
            s_xy_(dx, dy) = r_z(qz,dx,dy);
          }
        });
      });

      // Calculate Dxyz, xDyz, xyDz in plane
      RAJA::loop<thread1>(ctx, TBounds, [&](int qy) { 
         RAJA::loop<thread0>(ctx, TBounds, [&](int qx) {
          if ((qx < Q1D) && (qy < Q1D)) {
            double s = 0;
            for (int dy = 0; dy < D1D; ++dy) {
              const double wy = s_B_(qy, dy);
              for (int dx = 0; dx < D1D; ++dx) {
                const double wx = s_B_(qx, dx);
                s += wx * wy * s_xy_(dx, dy);
              }
            }

            s *= d(qx, qy, qz, e);

            for (int dz = 0; dz < D1D; ++dz) {
              const double wz  = s_Bt_(dz, qz);
              r_z2(dz,qx,qy) += wz * s;
            }
          }
       });
      });
      ctx.teamSync();
    }

    // Iterate over xy planes to compute solution
    for (int dz = 0; dz < D1D; ++dz) {

      // Place xy plane in @shared memory
      RAJA::loop<thread1>(ctx, TBounds, [&](int qy) { 
         RAJA::loop<thread0>(ctx, TBounds, [&](int qx) {
          if ((qx < Q1D) && (qy < Q1D)) {
            s_xy_(qx, qy) = r_z2(dz,qx,qy);
          }
        });
    });


      // Finalize solution in xy plane
      RAJA::loop<thread1>(ctx, TBounds, [&](int dy) { 
         RAJA::loop<thread0>(ctx, TBounds, [&](int dx) {
          if ((dx < D1D) && (dy < D1D)) {
            double solZ = 0;
            for (int qy = 0; qy < Q1D; ++qy) {
              const double wy = s_Bt_(dy, qy);
              for (int qx = 0; qx < Q1D; ++qx) {
                const double wx = s_Bt_(dx, qx);
                solZ += wx * wy * s_xy_(qx, qy);
              }
            }
            y(dx, dy, dz, e) += solZ;
          }

       });
     });

      ctx.teamSync();
    }//dz loop

      
  }); //element loop

 }); //launch

#else
   auto b = Reshape(b_.Read(), Q1D, D1D);
   RAJA::launch<launch_policy>(select_cpu_or_gpu,
                               RAJA::Resources(RAJA::Teams(NE),RAJA::Threads(Q1D, Q1D)),
   [=] RAJA_HOST_DEVICE (RAJA::LaunchContext ctx) {

   RAJA::loop<team0>(ctx, RAJA::RangeSegment(0, NE), [&](int e) {

      const int D1D = T_D1D ? T_D1D : d1d;
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      constexpr int MQ1 = T_Q1D ? T_Q1D : MAX_Q1D;
      constexpr int MD1 = T_D1D ? T_D1D : MAX_D1D;
      constexpr int MDQ = (MQ1 > MD1) ? MQ1 : MD1;
      MFEM_SHARED double sDQ[MQ1*MD1];
      double (*B)[MD1] = (double (*)[MD1]) sDQ;
      double (*Bt)[MQ1] = (double (*)[MQ1]) sDQ;
      MFEM_SHARED double sm0[MDQ*MDQ*MDQ];
      MFEM_SHARED double sm1[MDQ*MDQ*MDQ];
      double (*X)[MD1][MD1]   = (double (*)[MD1][MD1]) sm0;
      double (*DDQ)[MD1][MQ1] = (double (*)[MD1][MQ1]) sm1;
      double (*DQQ)[MQ1][MQ1] = (double (*)[MQ1][MQ1]) sm0;
      double (*QQQ)[MQ1][MQ1] = (double (*)[MQ1][MQ1]) sm1;
      double (*QQD)[MQ1][MD1] = (double (*)[MQ1][MD1]) sm0;
      double (*QDD)[MD1][MD1] = (double (*)[MD1][MD1]) sm1;

      RAJA::loop<thread1>(ctx, RAJA::RangeSegment(0, D1D), [&](int dy) {
         RAJA::loop<thread0>(ctx, RAJA::RangeSegment(0, D1D), [&](int dx) {

            MFEM_UNROLL(MD1)
            for (int dz = 0; dz < D1D; ++dz)
            {
               X[dz][dy][dx] = x(dx,dy,dz,e);
            }
           });

        RAJA::loop<thread0>(ctx, RAJA::RangeSegment(0, Q1D), [&](int dx) {
            B[dx][dy] = b(dx,dy);
          });
        });

      MFEM_SYNC_THREAD;
      RAJA::loop<thread1>(ctx, RAJA::RangeSegment(0, D1D), [&](int dy) {
         RAJA::loop<thread0>(ctx, RAJA::RangeSegment(0, Q1D), [&](int qx) {

            double u[D1D];
            MFEM_UNROLL(MD1)
            for (int dz = 0; dz < D1D; dz++)
            {
               u[dz] = 0;
            }
            MFEM_UNROLL(MD1)
            for (int dx = 0; dx < D1D; ++dx)
            {
               MFEM_UNROLL(MD1)
               for (int dz = 0; dz < D1D; ++dz)
               {
                  u[dz] += X[dz][dy][dx] * B[qx][dx];
               }
            }
            MFEM_UNROLL(MD1)
            for (int dz = 0; dz < D1D; ++dz)
            {
               DDQ[dz][dy][qx] = u[dz];
            }
           });
        });

      MFEM_SYNC_THREAD;

      RAJA::loop<thread1>(ctx, RAJA::RangeSegment(0, Q1D), [&](int qy) {
        RAJA::loop<thread0>(ctx, RAJA::RangeSegment(0, Q1D), [&](int qx) {

            double u[D1D];
            MFEM_UNROLL(MD1)
            for (int dz = 0; dz < D1D; dz++)
            {
               u[dz] = 0;
            }
            MFEM_UNROLL(MD1)
            for (int dy = 0; dy < D1D; ++dy)
            {
               MFEM_UNROLL(MD1)
               for (int dz = 0; dz < D1D; dz++)
               {
                  u[dz] += DDQ[dz][dy][qx] * B[qy][dy];
               }
            }
            MFEM_UNROLL(MD1)
            for (int dz = 0; dz < D1D; dz++)
            {
               DQQ[dz][qy][qx] = u[dz];
            }
          });
        });

     MFEM_SYNC_THREAD;
     RAJA::loop<thread1>(ctx, RAJA::RangeSegment(0, Q1D), [&](int qy) {
         RAJA::loop<thread0>(ctx, RAJA::RangeSegment(0, Q1D), [&](int qx) {

            double u[Q1D];
            MFEM_UNROLL(MQ1)
            for (int qz = 0; qz < Q1D; qz++)
            {
               u[qz] = 0;
            }
            MFEM_UNROLL(MD1)
            for (int dz = 0; dz < D1D; ++dz)
            {
               MFEM_UNROLL(MQ1)
               for (int qz = 0; qz < Q1D; qz++)
               {
                  u[qz] += DQQ[dz][qy][qx] * B[qz][dz];
               }
            }
            MFEM_UNROLL(MQ1)
            for (int qz = 0; qz < Q1D; qz++)
            {
               QQQ[qz][qy][qx] = u[qz] * d(qx,qy,qz,e);
            }
           });
       });
      MFEM_SYNC_THREAD;

     RAJA::loop<thread1>(ctx, RAJA::RangeSegment(0, D1D), [&](int d) {
         RAJA::loop<thread0>(ctx, RAJA::RangeSegment(0, Q1D), [&](int q) {
             Bt[d][q] = b(q,d);
           });
       });
      MFEM_SYNC_THREAD;

      RAJA::loop<thread1>(ctx, RAJA::RangeSegment(0, Q1D), [&](int qy) {
          RAJA::loop<thread0>(ctx, RAJA::RangeSegment(0, D1D), [&](int dx) {

            double u[Q1D];
            MFEM_UNROLL(MQ1)
            for (int qz = 0; qz < Q1D; ++qz)
            {
               u[qz] = 0;
            }
            MFEM_UNROLL(MQ1)
            for (int qx = 0; qx < Q1D; ++qx)
            {
               MFEM_UNROLL(MQ1)
               for (int qz = 0; qz < Q1D; ++qz)
               {
                  u[qz] += QQQ[qz][qy][qx] * Bt[dx][qx];
               }
            }
            MFEM_UNROLL(MQ1)
            for (int qz = 0; qz < Q1D; ++qz)
            {
               QQD[qz][qy][dx] = u[qz];
            }
            });
        });
      MFEM_SYNC_THREAD;

     RAJA::loop<thread1>(ctx, RAJA::RangeSegment(0, D1D), [&](int dy) {
           RAJA::loop<thread0>(ctx, RAJA::RangeSegment(0, D1D), [&](int dx) {

            double u[Q1D];
            MFEM_UNROLL(MQ1)
            for (int qz = 0; qz < Q1D; ++qz)
            {
               u[qz] = 0;
            }
            MFEM_UNROLL(MQ1)
            for (int qy = 0; qy < Q1D; ++qy)
            {
               MFEM_UNROLL(MQ1)
               for (int qz = 0; qz < Q1D; ++qz)
               {
                  u[qz] += QQD[qz][qy][dx] * Bt[dy][qy];
               }
            }
            MFEM_UNROLL(MQ1)
            for (int qz = 0; qz < Q1D; ++qz)
            {
               QDD[qz][dy][dx] = u[qz];
            }
             });
       });
      MFEM_SYNC_THREAD;

      RAJA::loop<thread1>(ctx, RAJA::RangeSegment(0, D1D), [&](int dy) {
        RAJA::loop<thread0>(ctx, RAJA::RangeSegment(0, D1D), [&](int dx) {
            double u[D1D];
            MFEM_UNROLL(MD1)
            for (int dz = 0; dz < D1D; ++dz)
            {
               u[dz] = 0;
            }
            MFEM_UNROLL(MQ1)
            for (int qz = 0; qz < Q1D; ++qz)
            {
               MFEM_UNROLL(MD1)
               for (int dz = 0; dz < D1D; ++dz)
               {
                  u[dz] += QDD[qz][dy][dx] * Bt[dz][qz];
               }
            }
            MFEM_UNROLL(MD1)
            for (int dz = 0; dz < D1D; ++dz)
            {
               y(dx,dy,dz,e) += u[dz];
            }
          });
        });

     });//element loop

   }); //launch
#endif

}



static void PAMassApply(const int dim,
                        const int D1D,
                        const int Q1D,
                        const int NE,
                        const Array<double> &B,
                        const Array<double> &Bt,
                        const Vector &D,
                        const Vector &X,
                        Vector &Y)
{

  /*
  //Error checking..
  printf("D1D %d Q1D %d \n", D1D, Q1D);
  Vector myY1(Y);
  Vector myY2(Y);

  RajaSmemPAMassApply3D<4,5>(NE,B,Bt,D,X,myY1);
  SmemPAMassApply3D<4,5>(NE,B,Bt,D,X,myY2);

   myY2 -= myY1;
   double error = myY2.Norml2();
   printf("error %g \n", error); 
   if(error > 1e-12) { exit(-1);}
  */
  

#ifdef MFEM_USE_OCCA
   if (DeviceCanUseOcca())
   {
      if (dim == 2)
      {
         return OccaPAMassApply2D(D1D,Q1D,NE,B,Bt,D,X,Y);
      }
      if (dim == 3)
      {
         return OccaPAMassApply3D(D1D,Q1D,NE,B,Bt,D,X,Y);
      }
      MFEM_ABORT("OCCA PA Mass Apply unknown kernel!");
   }
#endif // MFEM_USE_OCCA
   const int id = (D1D << 4) | Q1D;
   if (dim == 2)
   {
      switch (id)
      {
         case 0x22: return SmemPAMassApply2D<2,2,16>(NE,B,Bt,D,X,Y);
         case 0x24: return SmemPAMassApply2D<2,4,16>(NE,B,Bt,D,X,Y);
         case 0x33: return SmemPAMassApply2D<3,3,16>(NE,B,Bt,D,X,Y);
         case 0x34: return SmemPAMassApply2D<3,4,16>(NE,B,Bt,D,X,Y);
         case 0x36: return SmemPAMassApply2D<3,6,16>(NE,B,Bt,D,X,Y);
         case 0x44: return SmemPAMassApply2D<4,4,8>(NE,B,Bt,D,X,Y);
         case 0x48: return SmemPAMassApply2D<4,8,4>(NE,B,Bt,D,X,Y);
         case 0x55: return SmemPAMassApply2D<5,5,8>(NE,B,Bt,D,X,Y);
         case 0x58: return SmemPAMassApply2D<5,8,2>(NE,B,Bt,D,X,Y);
         case 0x66: return SmemPAMassApply2D<6,6,4>(NE,B,Bt,D,X,Y);
         case 0x77: return SmemPAMassApply2D<7,7,4>(NE,B,Bt,D,X,Y);
         case 0x88: return SmemPAMassApply2D<8,8,2>(NE,B,Bt,D,X,Y);
         case 0x99: return SmemPAMassApply2D<9,9,2>(NE,B,Bt,D,X,Y);
         default:   return PAMassApply2D(NE,B,Bt,D,X,Y,D1D,Q1D);
      }
   }
   else if (dim == 3)
   {

     if (Device::Allows(Backend::CUDA_MASK & ~Backend::CUDA) ||
         Device::Allows(Backend::CPU_MASK & ~Backend::CPU))
     {

      switch (id)
      {
         case 0x23: return RajaSmemPAMassApply3D<2,3>(NE,B,Bt,D,X,Y);
         case 0x24: return RajaSmemPAMassApply3D<2,4>(NE,B,Bt,D,X,Y);
         case 0x34: return RajaSmemPAMassApply3D<3,4>(NE,B,Bt,D,X,Y);
         case 0x36: return RajaSmemPAMassApply3D<3,6>(NE,B,Bt,D,X,Y);
         case 0x45: return RajaSmemPAMassApply3D<4,5>(NE,B,Bt,D,X,Y);
         case 0x46: return RajaSmemPAMassApply3D<4,6>(NE,B,Bt,D,X,Y);
         case 0x48: return RajaSmemPAMassApply3D<4,8>(NE,B,Bt,D,X,Y);
         case 0x56: return RajaSmemPAMassApply3D<5,6>(NE,B,Bt,D,X,Y);
         case 0x58: return RajaSmemPAMassApply3D<5,8>(NE,B,Bt,D,X,Y);
         case 0x67: return RajaSmemPAMassApply3D<6,7>(NE,B,Bt,D,X,Y);
         case 0x78: return RajaSmemPAMassApply3D<7,8>(NE,B,Bt,D,X,Y);
         case 0x89: return RajaSmemPAMassApply3D<8,9>(NE,B,Bt,D,X,Y);
         case 0x9A: return RajaSmemPAMassApply3D<9,10>(NE,B,Bt,D,X,Y);
         default: mfem_error("RAJA case not supported \n");
      }

     }
     else
     {

      switch (id)
      {
         case 0x23: return SmemPAMassApply3D<2,3>(NE,B,Bt,D,X,Y);
         case 0x24: return SmemPAMassApply3D<2,4>(NE,B,Bt,D,X,Y);
         case 0x34: return SmemPAMassApply3D<3,4>(NE,B,Bt,D,X,Y);
         case 0x36: return SmemPAMassApply3D<3,6>(NE,B,Bt,D,X,Y);
         case 0x45: return SmemPAMassApply3D<4,5>(NE,B,Bt,D,X,Y);
         case 0x46: return SmemPAMassApply3D<4,6>(NE,B,Bt,D,X,Y);
         case 0x48: return SmemPAMassApply3D<4,8>(NE,B,Bt,D,X,Y);
         case 0x56: return SmemPAMassApply3D<5,6>(NE,B,Bt,D,X,Y);
         case 0x58: return SmemPAMassApply3D<5,8>(NE,B,Bt,D,X,Y);
         case 0x67: return SmemPAMassApply3D<6,7>(NE,B,Bt,D,X,Y);
         case 0x78: return SmemPAMassApply3D<7,8>(NE,B,Bt,D,X,Y);
         case 0x89: return SmemPAMassApply3D<8,9>(NE,B,Bt,D,X,Y);
         case 0x9A: return SmemPAMassApply3D<9,10>(NE,B,Bt,D,X,Y);
         default:   return PAMassApply3D(NE,B,Bt,D,X,Y,D1D,Q1D);
      }

     }
   }
   mfem::out << "Unknown kernel 0x" << std::hex << id << std::endl;
   MFEM_ABORT("Unknown kernel.");
}

void MassIntegrator::AddMultPA(const Vector &x, Vector &y) const
{
#ifdef MFEM_USE_CEED
   if (DeviceCanUseCeed())
   {
      const CeedScalar *x_ptr;
      CeedScalar *y_ptr;
      CeedMemType mem;
      CeedGetPreferredMemType(internal::ceed, &mem);
      if ( Device::Allows(Backend::CUDA) && mem==CEED_MEM_DEVICE )
      {
         x_ptr = x.Read();
         y_ptr = y.ReadWrite();
      }
      else
      {
         x_ptr = x.HostRead();
         y_ptr = y.HostReadWrite();
         mem = CEED_MEM_HOST;
      }
      CeedVectorSetArray(ceedDataPtr->u, mem, CEED_USE_POINTER,
                         const_cast<CeedScalar*>(x_ptr));
      CeedVectorSetArray(ceedDataPtr->v, mem, CEED_USE_POINTER, y_ptr);

      CeedOperatorApplyAdd(ceedDataPtr->oper, ceedDataPtr->u, ceedDataPtr->v,
                           CEED_REQUEST_IMMEDIATE);

      CeedVectorSyncArray(ceedDataPtr->v, mem);
   }
   else
#endif
   {
      PAMassApply(dim, dofs1D, quad1D, ne, maps->B, maps->Bt, pa_data, x, y);
   }
}

} // namespace mfem
