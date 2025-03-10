// @HEADER
// ************************************************************************
//
//                           Intrepid2 Package
//                 Copyright (2007) Sandia Corporation
//
// Under terms of Contract DE-AC04-94AL85000, there is a non-exclusive
// license for use of this work by or on behalf of the U.S. Government.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY SANDIA CORPORATION "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SANDIA CORPORATION OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions? Contact Kyungjoo Kim  (kyukim@sandia.gov), or
//                    Mauro Perego  (mperego@sandia.gov)
//
// ************************************************************************
// @HEADER


/** \file
    \brief Test interpolation and projection capabilities for quadrilateral elements

    The test considers two quadrilaterals in the physical space sharing a common face.
    In order to test significant configurations, we consider 4 mappings of the reference quadrilateral
    to the first (physical) quadrilateral, so that the common face is mapped from all the 4 sides
    of the reference quadrilateral.
    Then, for each of the mappings, the global ids of the vertices of the common side are permuted.

    The test considers HGRAD, HCURL, HDIV and HVOL, of different degree, and for each of them checks that
    the Lagrangian interpolation, the interpolation-based projection, and the L2 projection, reproduce the
    target function to round-off errors when the target function is in the corresponding finite element space.

    Also, for the Lagrangian Interpolations, it checks that:
    1. that the Kronecker property holds for the oriented basis evaluated at the oriented DOF coordinates.
    2. that the basis coefficients located at the common faces/edges are the same when computed on the
       first and second quadrilateral.

    \author Created by Mauro Perego
 */

#include "Intrepid2_config.h"

#ifdef HAVE_INTREPID2_DEBUG
#define INTREPID2_TEST_FOR_DEBUG_ABORT_OVERRIDE_TO_CONTINUE
#endif

#include "Intrepid2_Orientation.hpp"
#include "Intrepid2_OrientationTools.hpp"
#include "Intrepid2_ProjectionTools.hpp"
#include "Intrepid2_HVOL_C0_FEM.hpp"
#include "Intrepid2_HGRAD_QUAD_C1_FEM.hpp"
#include "Intrepid2_HGRAD_QUAD_Cn_FEM.hpp"
#include "Intrepid2_HVOL_QUAD_Cn_FEM.hpp"
#include "Intrepid2_HCURL_QUAD_In_FEM.hpp"
#include "Intrepid2_PointTools.hpp"
#include "Intrepid2_CellTools.hpp"
#include "Intrepid2_FunctionSpaceTools.hpp"
#include "Intrepid2_LagrangianInterpolation.hpp"


#define Intrepid2_Experimental


#include "Teuchos_oblackholestream.hpp"
#include "Teuchos_RCP.hpp"
#include <array>
#include <set>
#include <random>
#include <algorithm>

namespace Intrepid2 {

namespace Test {

#define INTREPID2_TEST_ERROR_EXPECTED( S )              \
    try {                                                               \
      ++nthrow;                                                         \
      S ;                                                               \
    }                                                                   \
    catch (std::exception &err) {                                        \
      ++ncatch;                                                         \
      *outStream << "Expected Error ----------------------------------------------------------------\n"; \
      *outStream << err.what() << '\n';                                 \
      *outStream << "-------------------------------------------------------------------------------" << "\n\n"; \
    }

template<typename ValueType, typename DeviceType>
int InterpolationProjectionQuad(const bool verbose) {

  using ExecSpaceType = typename DeviceType::execution_space;
  using MemSpaceType = typename DeviceType::memory_space;

  using DynRankView = Kokkos::DynRankView<ValueType,DeviceType>;

#define ConstructWithLabel(obj, ...) obj(#obj, __VA_ARGS__)

  Teuchos::RCP<std::ostream> outStream;
  Teuchos::oblackholestream bhs; // outputs nothing

  if (verbose)
    outStream = Teuchos::rcp(&std::cout, false);
  else
    outStream = Teuchos::rcp(&bhs,       false);

  Teuchos::oblackholestream oldFormatState;
  oldFormatState.copyfmt(std::cout);

  using HostSpaceType = Kokkos::DefaultHostExecutionSpace;

  using DynRankViewIntHost = Kokkos::DynRankView<ordinal_type,HostSpaceType>;

  *outStream << "DeviceSpace::  ";   ExecSpaceType::print_configuration(*outStream, false);
  *outStream << "HostSpace::    ";   HostSpaceType::print_configuration(*outStream, false);
  *outStream << "\n";

  int errorFlag = 0;
  const ValueType tol = tolerence();

  //target functions and their derivatives

  struct Fun {
    ValueType
    KOKKOS_INLINE_FUNCTION
    operator()(const ordinal_type& degree, const ValueType& x, const ValueType& y) {
      return std::pow(x+y,degree-1)*(x+y-2);
    }
  };

  struct FunDiv {
    ValueType
    KOKKOS_INLINE_FUNCTION
    operator()(const ordinal_type& degree, const ValueType& x, const ValueType& y, const int comp) {
      ValueType a = std::pow(x-y, degree-1);
      ValueType f0 = 3;
      ValueType f1 = std::pow(y, degree-1);
      //fun = f + a x
      switch (comp) {
      case 0:
        return f0 + a*x;
      case 1:
        return f1 + a*y;
      default:
        return 0;
      }
    }
  };

  struct FunCurl {
    ValueType
    KOKKOS_INLINE_FUNCTION
    operator()(const ordinal_type& degree, const ValueType& x, const ValueType& y, const int comp) {
      ValueType a = std::pow(x-1, degree-1);
      ValueType f0 = 3;
      ValueType f1 = std::pow(y, degree-1);
      //fun = f + a \times x
      switch (comp) {
      case 0:
        return f0 + -a*y;
      case 1:
        return f1 + a*x;
      default:
        return 0;
      }
    }
  };

  typedef std::array<ordinal_type,2> edgeType;
  typedef CellTools<DeviceType> ct;
  typedef OrientationTools<DeviceType> ots;
  typedef Experimental::ProjectionTools<DeviceType> pts;
  typedef FunctionSpaceTools<DeviceType> fst;
  typedef Experimental::LagrangianInterpolation<DeviceType> li;
  using  basisType = Basis<DeviceType,ValueType,ValueType>;

  constexpr ordinal_type dim = 2;
  constexpr ordinal_type numCells = 2;
  constexpr ordinal_type numElemVertexes = 4;
  constexpr ordinal_type numTotalVertexes = 6;

  ValueType  vertices_orig[numTotalVertexes][dim] = {{-1,-1},{1,-1},{1,1},{-1,1}, {-1,2},{1,2}};
  ordinal_type cells_orig[numCells][numElemVertexes] = {{0,1,2,3},{2,3,4,5}};
  edgeType common_edge = {{2,3}};
  ordinal_type cells_rotated[numCells][numElemVertexes];

  const ordinal_type max_degree = 4;

  using CG_NBasis = NodalBasisFamily<DeviceType,ValueType,ValueType>;
  using CG_DNBasis = DerivedNodalBasisFamily<DeviceType,ValueType,ValueType>;
  std::vector<basisType*> basis_set;

  shards::CellTopology cellTopo(shards::getCellTopologyData<shards::Quadrilateral<4> >());
  ordinal_type numNodesPerElem = cellTopo.getNodeCount();

  *outStream
  << "===============================================================================\n"
  << "|                                                                             |\n"
  << "|                 Test 1 (Orientation - HGRAD)                                |\n"
  << "|                                                                             |\n"
  << "===============================================================================\n";


  try {

    //reordering of nodes to explore different orientations

    ordinal_type reorder[numTotalVertexes] = {0,1,2,3,4,5};

    do {
      ordinal_type orderback[numTotalVertexes];
      for(ordinal_type i=0;i<numTotalVertexes;++i) {
        orderback[reorder[i]]=i;
      }
      ValueType vertices[numTotalVertexes][dim];
      ordinal_type cells[numCells][numElemVertexes];
      std::copy(&cells_orig[0][0], &cells_orig[0][0]+numCells*numElemVertexes, &cells_rotated[0][0]);

      for (ordinal_type shift=0; shift<4; ++shift) {
        std::rotate_copy(&cells_orig[0][0], &cells_orig[0][0]+shift, &cells_orig[0][0]+4, &cells_rotated[0][0]);
        for(ordinal_type i=0; i<numCells;++i)
          for(ordinal_type j=0; j<numElemVertexes;++j)
            cells[i][j] = reorder[cells_rotated[i][j]];

        for(ordinal_type i=0; i<numTotalVertexes;++i)
          for(ordinal_type d=0; d<dim;++d)
            vertices[i][d] = vertices_orig[orderback[i]][d];

        *outStream <<  "Considering Quad 0: [ ";
        for(ordinal_type j=0; j<numElemVertexes;++j)
          *outStream << cells[0][j] << " ";
        *outStream << "] and Quad 1: [ ";
        for(ordinal_type j=0; j<numElemVertexes;++j)
          *outStream << cells[1][j] << " ";
        *outStream << "]\n";

        //computing vertices coords
        DynRankView ConstructWithLabel(physVertexes, numCells, numNodesPerElem, dim);
        auto hostPhysVertexes = Kokkos::create_mirror_view(physVertexes);
        for(ordinal_type i=0; i<numCells; ++i)
          for(ordinal_type j=0; j<numNodesPerElem; ++j)
            for(ordinal_type k=0; k<dim; ++k)
              hostPhysVertexes(i,j,k) = vertices[cells[i][j]][k];
        deep_copy(physVertexes, hostPhysVertexes);

        //computing common and edge
        ordinal_type edgeIndex[numCells];

        {
          edgeType edge={};
          for(ordinal_type i=0; i<numCells; ++i) {
            //compute common edge
            for (std::size_t ie=0; ie<cellTopo.getSideCount(); ++ie) {
              for (std::size_t k=0; k<cellTopo.getNodeCount(1,ie); ++k)
                edge[k]= cells_rotated[i][cellTopo.getNodeMap(1,ie,k)];

              if(edge == common_edge) edgeIndex[i]=ie;
            }
          }
        }

        // compute orientations for cells (one time computation)
        DynRankViewIntHost elemNodesHost(&cells[0][0], numCells, numElemVertexes);
        auto elemNodes = Kokkos::create_mirror_view_and_copy(MemSpaceType(),elemNodesHost);
        Kokkos::DynRankView<Orientation,DeviceType> elemOrts("elemOrts", numCells);
        ots::getOrientation(elemOrts, elemNodes, cellTopo);

        for (ordinal_type degree=1; degree <= max_degree; degree++) {
          basis_set.clear();
          if(degree==1)
            basis_set.push_back(new Basis_HGRAD_QUAD_C1_FEM<DeviceType,ValueType,ValueType>());
          basis_set.push_back(new typename  CG_NBasis::HGRAD_QUAD(degree,POINTTYPE_WARPBLEND));
          basis_set.push_back(new typename  CG_DNBasis::HGRAD_QUAD(degree,POINTTYPE_EQUISPACED));

          for (auto basisPtr:basis_set) {

            auto name = basisPtr->getName();
            *outStream << " " << name <<  ": " << degree << std::endl;
            ordinal_type basisCardinality = basisPtr->getCardinality();

            //compute DofCoords Oriented
            DynRankView ConstructWithLabel(dofCoordsOriented, numCells, basisCardinality, dim);
            DynRankView ConstructWithLabel(dofCoeffsPhys, numCells, basisCardinality);
            DynRankView ConstructWithLabel(physDofCoords, numCells, basisCardinality, dim);
            DynRankView ConstructWithLabel(funAtDofCoords, numCells, basisCardinality);
            DynRankView ConstructWithLabel(basisCoeffsLI, numCells, basisCardinality);

            //compute Lagrangian Interpolation of fun
            {
              li::getDofCoordsAndCoeffs(dofCoordsOriented, dofCoeffsPhys, basisPtr, elemOrts);

              //Compute physical Dof Coordinates
              DynRankView ConstructWithLabel(linearBasisValuesAtDofCoord, numCells, numNodesPerElem);
              Kokkos::parallel_for(Kokkos::RangePolicy<ExecSpaceType>(0,numCells),
              KOKKOS_LAMBDA (const int &i) {
                Fun fun;
                auto basisValuesAtEvalDofCoord = Kokkos::subview(linearBasisValuesAtDofCoord,i,Kokkos::ALL());
                for(ordinal_type j=0; j<basisCardinality; ++j){
                  auto evalPoint = Kokkos::subview(dofCoordsOriented,i,j,Kokkos::ALL());
                  Impl::Basis_HGRAD_QUAD_C1_FEM::template Serial<OPERATOR_VALUE>::getValues(basisValuesAtEvalDofCoord, evalPoint);
                  for(ordinal_type k=0; k<numNodesPerElem; ++k)
                    for(ordinal_type d=0; d<dim; ++d)
                      physDofCoords(i,j,d) += physVertexes(i,k,d)*basisValuesAtEvalDofCoord(k);

                  funAtDofCoords(i,j) += fun(degree, physDofCoords(i,j,0), physDofCoords(i,j,1));
                }
              });

              li::getBasisCoeffs(basisCoeffsLI, funAtDofCoords, dofCoeffsPhys);
              Kokkos::fence();
            }

            //Testing Kronecker property of basis functions
            {
              for(ordinal_type i=0; i<numCells; ++i) {
                DynRankView ConstructWithLabel(basisValuesAtDofCoords, numCells, basisCardinality, basisCardinality);
                DynRankView ConstructWithLabel(basisValuesAtDofCoordsOriented, numCells, basisCardinality, basisCardinality);
                DynRankView ConstructWithLabel(transformedBasisValuesAtDofCoordsOriented, numCells, basisCardinality, basisCardinality);
                auto inView = Kokkos::subview( dofCoordsOriented,i,Kokkos::ALL(),Kokkos::ALL());
                auto outView =Kokkos::subview( basisValuesAtDofCoords,i,Kokkos::ALL(),Kokkos::ALL());
                basisPtr->getValues(outView, inView);

                // modify basis values to account for orientations
                ots::modifyBasisByOrientation(basisValuesAtDofCoordsOriented,
                    basisValuesAtDofCoords,
                    elemOrts,
                    basisPtr);

                // transform basis values
                fst::HGRADtransformVALUE(transformedBasisValuesAtDofCoordsOriented,
                    basisValuesAtDofCoordsOriented);


                auto hostBasisValues = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), transformedBasisValuesAtDofCoordsOriented);
                auto hostDofCoeffsPhys = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), dofCoeffsPhys);
                ExecSpaceType().fence();
                for(ordinal_type k=0; k<basisCardinality; ++k) {
                  for(ordinal_type j=0; j<basisCardinality; ++j){
                    ValueType dofValue = hostBasisValues(i,k,j) * hostDofCoeffsPhys(i,j);
                    if ( k==j && std::abs( dofValue - 1.0 ) > 100*tol ) {
                      errorFlag++;
                      *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                      *outStream << " Basis function " << k << " of cell " << i << " does not have unit value at its node (" << dofValue <<")\n";
                    }
                    if ( k!=j && std::abs( dofValue ) > tol ) {
                      errorFlag++;
                      *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                      *outStream << " Basis function " << k << " of cell " << i << " does not vanish at node " << j << "(" << dofValue <<")\n";
                    }
                  }
                }
              }
            }

            //check that fun values are consistent on common edges dofs
            auto hostBasisCoeffsLI = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), basisCoeffsLI);
            {
              bool areDifferent(false);
              auto numEdgeDOFs = basisPtr->getDofCount(1,0);
              for(ordinal_type j=0;j<numEdgeDOFs && !areDifferent;j++) {
                areDifferent = std::abs(hostBasisCoeffsLI(0,basisPtr->getDofOrdinal(1,edgeIndex[0],j))
                    - hostBasisCoeffsLI(1,basisPtr->getDofOrdinal(1,edgeIndex[1],j))) > 10*tol;
              }

              if(areDifferent) {
                errorFlag++;
                *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                *outStream << "Function DOFs on common edge computed using Quad 0 basis functions are not consistent with those computed using Quad 1\n";
                *outStream << "Function DOFs for Quad 0 are:";
                for(ordinal_type j=0;j<numEdgeDOFs;j++)
                  *outStream << " " << hostBasisCoeffsLI(0,basisPtr->getDofOrdinal(1,edgeIndex[0],j));
                *outStream << "\nFunction DOFs for Quad 1 are:";
                for(ordinal_type j=0;j<numEdgeDOFs;j++)
                  *outStream << " " << hostBasisCoeffsLI(1,basisPtr->getDofOrdinal(1,edgeIndex[1],j));
                *outStream << std::endl;
              }
            }

            //check that fun values at reference points coincide with those computed using basis functions
            DynRankView ConstructWithLabel(basisValuesAtDofCoordsOriented, numCells, basisCardinality, basisCardinality);
            DynRankView ConstructWithLabel(transformedBasisValuesAtDofCoordsOriented, numCells, basisCardinality, basisCardinality);
            DynRankView basisValuesAtDofCoordsCells("inValues", numCells, basisCardinality, basisCardinality);

            for (ordinal_type ic = 0; ic < numCells; ++ic)
              basisPtr->getValues(Kokkos::subview(basisValuesAtDofCoordsCells, ic, Kokkos::ALL(), Kokkos::ALL()), Kokkos::subview(dofCoordsOriented, ic, Kokkos::ALL(), Kokkos::ALL()));

            // modify basis values to account for orientations
            ots::modifyBasisByOrientation(basisValuesAtDofCoordsOriented,
                basisValuesAtDofCoordsCells,
                elemOrts,
                basisPtr);

            // transform basis (pullback)
            fst::HGRADtransformVALUE(transformedBasisValuesAtDofCoordsOriented,
                basisValuesAtDofCoordsOriented);

            DynRankView ConstructWithLabel(funAtDofCoordsOriented, numCells, basisCardinality);
            Kokkos::parallel_for(Kokkos::RangePolicy<ExecSpaceType>(0,numCells),
              KOKKOS_LAMBDA (const int &i) {
              for(ordinal_type j=0; j<basisCardinality; ++j)
                for(ordinal_type k=0; k<basisCardinality; ++k)
                  funAtDofCoordsOriented(i,j) += basisCoeffsLI(i,k)*transformedBasisValuesAtDofCoordsOriented(i,k,j);
            });

            auto hostFunAtDofCoordsOriented = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), funAtDofCoordsOriented);
            auto hostFunAtDofCoords = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), funAtDofCoords);
            for(ordinal_type i=0; i<numCells; ++i) {
              ValueType error=0;
              for(ordinal_type j=0; j<basisCardinality; ++j) {
                error = std::max(std::abs( hostFunAtDofCoords(i,j) - hostFunAtDofCoordsOriented(i,j)), error);
              }

              if(error>100*tol) {
                errorFlag++;
                *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                *outStream << "Function values at reference points differ from those computed using basis functions of Quad " << i << "\n";
                *outStream << "Function values at reference points are:\n";
                for(ordinal_type j=0; j<basisCardinality; ++j)
                  *outStream << " (" << hostFunAtDofCoords(i,j)  << ")";
                *outStream << "\nFunction values at reference points computed using basis functions are\n";
                for(ordinal_type j=0; j<basisCardinality; ++j)
                  *outStream << " (" << hostFunAtDofCoordsOriented(i,j)  << ")";
                *outStream << std::endl;
              }
            }

            //compute projection-based interpolation of the Lagrangian interpolation
            DynRankView ConstructWithLabel(basisCoeffsHGrad, numCells, basisCardinality);
            {
              ordinal_type targetCubDegree(basisPtr->getDegree()),targetDerivCubDegree(basisPtr->getDegree());

              Experimental::ProjectionStruct<DeviceType,ValueType> projStruct;
              projStruct.createHGradProjectionStruct(basisPtr, targetCubDegree, targetDerivCubDegree);

              ordinal_type numPoints = projStruct.getNumTargetEvalPoints(), numGradPoints = projStruct.getNumTargetDerivEvalPoints();
              DynRankView ConstructWithLabel(evaluationPoints, numCells, numPoints, dim);
              DynRankView ConstructWithLabel(evaluationGradPoints, numCells, numGradPoints, dim);


              pts::getHGradEvaluationPoints(evaluationPoints,
                  evaluationGradPoints,
                  elemOrts,
                  basisPtr,
                  &projStruct);


              DynRankView ConstructWithLabel(targetAtEvalPoints, numCells, numPoints);
              DynRankView ConstructWithLabel(targetGradAtEvalPoints, numCells, numGradPoints, dim);

              DynRankView ConstructWithLabel(hgradBasisAtEvaluationPoints, numCells, basisCardinality , numPoints);
              DynRankView ConstructWithLabel(hgradBasisAtEvaluationPointsNonOriented, numCells, basisCardinality , numPoints);
              for(int ic=0; ic<numCells; ic++)
                basisPtr->getValues(Kokkos::subview(hgradBasisAtEvaluationPointsNonOriented, ic, Kokkos::ALL(), Kokkos::ALL()), Kokkos::subview(evaluationPoints, ic, Kokkos::ALL(), Kokkos::ALL()), OPERATOR_VALUE);
              ots::modifyBasisByOrientation(hgradBasisAtEvaluationPoints,
                  hgradBasisAtEvaluationPointsNonOriented,
                  elemOrts,
                  basisPtr);

              DynRankView ConstructWithLabel(gradOfHGradBasisAtEvaluationPoints, numCells, basisCardinality , numGradPoints, dim);
              if(numGradPoints>0) {
                DynRankView ConstructWithLabel(gradOfHGradBasisAtEvaluationPointsNonOriented, numCells, basisCardinality , numGradPoints, dim);
                for(int ic=0; ic<numCells; ic++)
                  basisPtr->getValues(Kokkos::subview(gradOfHGradBasisAtEvaluationPointsNonOriented, ic, Kokkos::ALL(), Kokkos::ALL(), Kokkos::ALL()), Kokkos::subview(evaluationGradPoints, ic, Kokkos::ALL(), Kokkos::ALL()), OPERATOR_GRAD);
                ots::modifyBasisByOrientation(gradOfHGradBasisAtEvaluationPoints,
                    gradOfHGradBasisAtEvaluationPointsNonOriented,
                    elemOrts,
                    basisPtr);
              }


              Kokkos::parallel_for(Kokkos::RangePolicy<ExecSpaceType>(0,numCells),
              KOKKOS_LAMBDA (const int &ic) {
                for(int i=0;i<numPoints;i++) {
                  for(int k=0;k<basisCardinality;k++)
                    targetAtEvalPoints(ic,i) += basisCoeffsLI(ic,k)*hgradBasisAtEvaluationPoints(ic,k,i);
                }
                for(int i=0;i<numGradPoints;i++) {
                  for(int k=0;k<basisCardinality;k++)
                    for(int d=0;d<dim;d++)
                      targetGradAtEvalPoints(ic,i,d) += basisCoeffsLI(ic,k)*gradOfHGradBasisAtEvaluationPoints(ic,k,i,d);//funHGradCoeffs(k)
                }
              });

              pts::getHGradBasisCoeffs(basisCoeffsHGrad,
                  targetAtEvalPoints,
                  targetGradAtEvalPoints,
                  evaluationPoints,
                  evaluationGradPoints,
                  elemOrts,
                  basisPtr,
                  &projStruct);
            }

            //check that the basis coefficients of the Lagrangian nterpolation are the same as those of the projection-based interpolation
            {
              ValueType diffErr(0);
              auto hostBasisCoeffsHGrad = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), basisCoeffsHGrad);
              for(int k=0;k<basisCardinality;k++) {
                for(int ic=0; ic<numCells; ic++)
                  diffErr = std::max(diffErr, std::abs(hostBasisCoeffsLI(ic,k) - hostBasisCoeffsHGrad(ic,k)));
              }

              if(diffErr > pow(7, degree-1)*tol) { //heuristic relation on how round-off error depends on degree
                errorFlag++;
                *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                *outStream << "HGRAD_C" << degree << ": The weights recovered with the optimization are different than the one used for generating the functon."<<
                    "\nThe max The infinite norm of the difference between the weights is: " <<  diffErr << std::endl;
              }
            }

            //compute L2 projection of the Lagrangian interpolation
            DynRankView ConstructWithLabel(basisCoeffsL2, numCells, basisCardinality);
            {
              ordinal_type targetCubDegree(basisPtr->getDegree());

              Experimental::ProjectionStruct<DeviceType,ValueType> projStruct;
              projStruct.createL2ProjectionStruct(basisPtr, targetCubDegree);

              ordinal_type numPoints = projStruct.getNumTargetEvalPoints();
              DynRankView ConstructWithLabel(evaluationPoints, numCells, numPoints, dim);

              pts::getL2EvaluationPoints(evaluationPoints,
                  elemOrts,
                  basisPtr,
                  &projStruct);


              DynRankView ConstructWithLabel(targetAtEvalPoints, numCells, numPoints);
              DynRankView ConstructWithLabel(hgradBasisAtEvaluationPoints, numCells, basisCardinality , numPoints);
              DynRankView ConstructWithLabel(hgradBasisAtEvaluationPointsNonOriented, numCells, basisCardinality , numPoints);
              for(int ic=0; ic<numCells; ic++)
                basisPtr->getValues(Kokkos::subview(hgradBasisAtEvaluationPointsNonOriented, ic, Kokkos::ALL(), Kokkos::ALL()), Kokkos::subview(evaluationPoints, ic, Kokkos::ALL(), Kokkos::ALL()), OPERATOR_VALUE);
              ots::modifyBasisByOrientation(hgradBasisAtEvaluationPoints,
                  hgradBasisAtEvaluationPointsNonOriented,
                  elemOrts,
                  basisPtr);


              Kokkos::parallel_for(Kokkos::RangePolicy<ExecSpaceType>(0,numCells),
              KOKKOS_LAMBDA (const int &ic) {
                for(int i=0;i<numPoints;i++) {
                  for(int k=0;k<basisCardinality;k++)
                    targetAtEvalPoints(ic,i) += basisCoeffsLI(ic,k)*hgradBasisAtEvaluationPoints(ic,k,i);
                }
              });

              pts::getL2BasisCoeffs(basisCoeffsL2,
                  targetAtEvalPoints,
                  evaluationPoints,
                  elemOrts,
                  basisPtr,
                  &projStruct);
            }
            //check that the basis coefficients of the Lagrangian interpolation are the same as those of the L2 projection
            {
              ValueType diffErr =0;
              auto hostBasisCoeffsL2 = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), basisCoeffsL2);

              for(int k=0;k<basisCardinality;k++) {
                for(int ic=0; ic<numCells; ic++)
                  diffErr = std::max(diffErr, std::abs(hostBasisCoeffsLI(ic,k) - hostBasisCoeffsL2(ic,k)));
              }

              if(diffErr > pow(7, degree-1)*tol) { //heuristic relation on how round-off error depends on degree
                errorFlag++;
                *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                *outStream << "HGRAD_C" << degree << ": The weights recovered with the L2 optimization are different than the one used for generating the functon."<<
                    "\nThe max The infinite norm of the difference between the weights is: " <<  diffErr << std::endl;
              }
            }

            //compute Broken L2 projection of the Lagrangian interpolation
            DynRankView ConstructWithLabel(basisCoeffsL2DG, numCells, basisCardinality);
            {
              ordinal_type targetCubDegree(basisPtr->getDegree());

              Experimental::ProjectionStruct<DeviceType,ValueType> projStruct;
              projStruct.createL2DGProjectionStruct(basisPtr, targetCubDegree);

              ordinal_type numPoints = projStruct.getNumTargetEvalPoints();
              DynRankView ConstructWithLabel(evaluationPoints, numCells, numPoints, dim);

              pts::getL2DGEvaluationPoints(evaluationPoints,
                  basisPtr,
                  &projStruct);

              DynRankView ConstructWithLabel(targetAtEvalPoints, numCells, numPoints);
              DynRankView ConstructWithLabel(hgradBasisAtEvaluationPoints, numCells, basisCardinality , numPoints);
              DynRankView ConstructWithLabel(hgradBasisAtEvaluationPointsNonOriented, numCells, basisCardinality , numPoints);
              for(int ic=0; ic<numCells; ic++)
                basisPtr->getValues(Kokkos::subview(hgradBasisAtEvaluationPointsNonOriented, ic, Kokkos::ALL(), Kokkos::ALL()), Kokkos::subview(evaluationPoints, ic, Kokkos::ALL(), Kokkos::ALL()), OPERATOR_VALUE);
              ots::modifyBasisByOrientation(hgradBasisAtEvaluationPoints,
                  hgradBasisAtEvaluationPointsNonOriented,
                  elemOrts,
                  basisPtr);

              Kokkos::parallel_for(Kokkos::RangePolicy<ExecSpaceType>(0,numCells),
              KOKKOS_LAMBDA (const int &ic) {
                for(int i=0;i<numPoints;i++) {
                  for(int k=0;k<basisCardinality;k++)
                    targetAtEvalPoints(ic,i) += basisCoeffsLI(ic,k)*hgradBasisAtEvaluationPoints(ic,k,i);
                }
              });

              pts::getL2DGBasisCoeffs(basisCoeffsL2DG,
                  targetAtEvalPoints,
                  elemOrts,
                  basisPtr,
                  &projStruct);
            }

            //check that the basis coefficients of the Lagrangian interpolation are the same as those of the L2 projection
            {
              ValueType diffErr =0;
              auto hostBasisCoeffsL2DG = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), basisCoeffsL2DG);
              for(int k=0;k<basisCardinality;k++) {
                for(int ic=0; ic<numCells; ic++)
                  diffErr = std::max(diffErr, std::abs(hostBasisCoeffsLI(ic,k) - hostBasisCoeffsL2DG(ic,k)));
              }

              if(diffErr > pow(7, degree-1)*tol) { //heuristic relation on how round-off error depends on degree
                errorFlag++;
                *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                *outStream << "HGRAD_C" << degree << ": The weights recovered with the L2DG optimization are different than the one used for generating the functon."<<
                    "\nThe max The infinite norm of the difference between the weights is: " <<  diffErr << std::endl;
              }
            }
            delete basisPtr;
          }
        }
      }
    } while(std::next_permutation(&reorder[0]+1, &reorder[0]+4)); //reorder vertices of common face

  } catch (std::exception &err) {
    std::cout << " Exeption\n";
    *outStream << err.what() << "\n\n";
    errorFlag = -1000;
  }




  *outStream
  << "===============================================================================\n"
  << "|                                                                             |\n"
  << "|                 Test 2 (Orientation - HCURL)                                |\n"
  << "|                                                                             |\n"
  << "===============================================================================\n";


  try {

    ordinal_type reorder[numTotalVertexes] = {0,1,2,3,4,5};

    do {
      ordinal_type orderback[numTotalVertexes];
      for(ordinal_type i=0;i<numTotalVertexes;++i) {
        orderback[reorder[i]]=i;
      }
      ValueType vertices[numTotalVertexes][dim];
      ordinal_type cells[numCells][numElemVertexes];
      std::copy(&cells_orig[0][0], &cells_orig[0][0]+numCells*numElemVertexes, &cells_rotated[0][0]);

      for (ordinal_type shift=0; shift<4; ++shift) {
        std::rotate_copy(&cells_orig[0][0], &cells_orig[0][0]+shift, &cells_orig[0][0]+4, &cells_rotated[0][0]);
        for(ordinal_type i=0; i<numCells;++i)
          for(ordinal_type j=0; j<numElemVertexes;++j)
            cells[i][j] = reorder[cells_rotated[i][j]];

        for(ordinal_type i=0; i<numTotalVertexes;++i)
          for(ordinal_type d=0; d<dim;++d)
            vertices[i][d] = vertices_orig[orderback[i]][d];

        *outStream <<  "Considering Quad 0: [ ";
        for(ordinal_type j=0; j<numElemVertexes;++j)
          *outStream << cells[0][j] << " ";
        *outStream << "] and Quad 1: [ ";
        for(ordinal_type j=0; j<numElemVertexes;++j)
          *outStream << cells[1][j] << " ";
        *outStream << "]\n";

        //computing vertices coords
        DynRankView ConstructWithLabel(physVertexes, numCells, numNodesPerElem, dim);
        auto hostPhysVertexes = Kokkos::create_mirror_view(physVertexes);
        for(ordinal_type i=0; i<numCells; ++i)
          for(ordinal_type j=0; j<numNodesPerElem; ++j)
            for(ordinal_type k=0; k<dim; ++k)
              hostPhysVertexes(i,j,k) = vertices[cells[i][j]][k];
        deep_copy(physVertexes, hostPhysVertexes);

        ordinal_type edgeIndex[numCells];
        {
          edgeType edge={};
          //bool edgeOrientation[numCells][4];
          for(ordinal_type i=0; i<numCells; ++i) {

            //compute edges' tangents
            for (std::size_t ie=0; ie<cellTopo.getEdgeCount(); ++ie) {
              for (std::size_t k=0; k<cellTopo.getNodeCount(1,ie); ++k)
                edge[k]= cells_rotated[i][cellTopo.getNodeMap(1,ie,k)];

              //compute common edge
              if(edge == common_edge)
                edgeIndex[i]=ie;
            }
          }
        }

        // compute orientations for cells (one time computation)
        DynRankViewIntHost elemNodesHost(&cells[0][0], numCells, numElemVertexes);
        auto elemNodes = Kokkos::create_mirror_view_and_copy(MemSpaceType(),elemNodesHost);
        Kokkos::DynRankView<Orientation,DeviceType> elemOrts("elemOrts", numCells);
        ots::getOrientation(elemOrts, elemNodes, cellTopo);

        for (ordinal_type degree=1; degree <= max_degree; degree++) {

          basis_set.clear();
          if(degree==1)
            basis_set.push_back(new Basis_HCURL_QUAD_I1_FEM<DeviceType,ValueType,ValueType>());
          basis_set.push_back(new typename  CG_NBasis::HCURL_QUAD(degree,POINTTYPE_EQUISPACED));
          basis_set.push_back(new typename  CG_DNBasis::HCURL_QUAD(degree,POINTTYPE_WARPBLEND));

          for (auto basisPtr:basis_set) {

            auto name = basisPtr->getName();
            *outStream << " " << name <<  ": " << degree << std::endl;

            ordinal_type basisCardinality = basisPtr->getCardinality();

            //compute DofCoords Oriented
            DynRankView ConstructWithLabel(dofCoordsOriented, numCells, basisCardinality, dim);
            DynRankView ConstructWithLabel(physDofCoords, numCells, basisCardinality, dim);
            DynRankView ConstructWithLabel(funAtDofCoords, numCells, basisCardinality, dim);
            DynRankView ConstructWithLabel(dofCoeffs, numCells, basisCardinality, dim);
            DynRankView ConstructWithLabel(basisCoeffsLI, numCells, basisCardinality);

            //compute Lagrangian Interpolation of fun
            {
              li::getDofCoordsAndCoeffs(dofCoordsOriented, dofCoeffs, basisPtr, elemOrts);

              //Compute physical Dof Coordinates

              DynRankView ConstructWithLabel(jacobian, numCells, basisCardinality, dim, dim);
              ct::setJacobian(jacobian, dofCoordsOriented, physVertexes, cellTopo);

              DynRankView ConstructWithLabel(linearBasisValuesAtDofCoord, numCells, numNodesPerElem);
              DynRankView ConstructWithLabel(fwdFunAtDofCoords, numCells, basisCardinality, dim);
              Kokkos::parallel_for(Kokkos::RangePolicy<ExecSpaceType>(0,numCells),
              KOKKOS_LAMBDA (const int &i) {
                FunCurl fun;
                auto basisValuesAtEvalDofCoord = Kokkos::subview(linearBasisValuesAtDofCoord,i,Kokkos::ALL());
                for(ordinal_type j=0; j<basisCardinality; ++j){
                  auto evalPoint = Kokkos::subview(dofCoordsOriented,i,j,Kokkos::ALL());
                  Impl::Basis_HGRAD_QUAD_C1_FEM::template Serial<OPERATOR_VALUE>::getValues(basisValuesAtEvalDofCoord, evalPoint);
                  for(ordinal_type k=0; k<numNodesPerElem; ++k)
                    for(ordinal_type d=0; d<dim; ++d)
                      physDofCoords(i,j,d) += physVertexes(i,k,d)*basisValuesAtEvalDofCoord(k);

                  for(ordinal_type k=0; k<dim; ++k)
                    funAtDofCoords(i,j,k) = fun(degree, physDofCoords(i,j,0), physDofCoords(i,j,1), k);
                  for(ordinal_type k=0; k<dim; ++k)
                    for(ordinal_type d=0; d<dim; ++d)
                      fwdFunAtDofCoords(i,j,k) += jacobian(i,j,d,k)*funAtDofCoords(i,j,d);
                }
              });

              li::getBasisCoeffs(basisCoeffsLI, fwdFunAtDofCoords, dofCoeffs);
            }


            //Testing Kronecker property of basis functions
            {
              for(ordinal_type i=0; i<numCells; ++i) {
                DynRankView ConstructWithLabel(basisValuesAtDofCoords, numCells, basisCardinality, basisCardinality, dim);
                DynRankView ConstructWithLabel(basisValuesAtDofCoordsOriented, numCells, basisCardinality, basisCardinality, dim);
                auto inView = Kokkos::subview( dofCoordsOriented,i,Kokkos::ALL(),Kokkos::ALL());
                auto outView =Kokkos::subview( basisValuesAtDofCoords,i,Kokkos::ALL(),Kokkos::ALL(),Kokkos::ALL());
                basisPtr->getValues(outView, inView);

                // modify basis values to account for orientations
                ots::modifyBasisByOrientation(basisValuesAtDofCoordsOriented,
                    basisValuesAtDofCoords,
                    elemOrts,
                    basisPtr);

                auto hostBasisValues = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), basisValuesAtDofCoordsOriented);
                auto hostDofCoeffs = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), dofCoeffs);
                for(ordinal_type k=0; k<basisCardinality; ++k) {
                  for(ordinal_type j=0; j<basisCardinality; ++j){
                    ValueType dofValue=0;
                    for(ordinal_type d=0; d<dim; ++d)
                      dofValue += hostBasisValues(i,k,j,d) * hostDofCoeffs(i,j,d);
                    if ( k==j && std::abs( dofValue - 1.0 ) > 100*tol ) {
                      errorFlag++;
                      *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                      *outStream << " Basis function " << k << " of cell " << i << " does not have unit value at its node (" << dofValue <<")\n";
                    }
                    if ( k!=j && std::abs( dofValue ) > tol ) {
                      errorFlag++;
                      *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                      *outStream << " Basis function " << k << " of cell " << i << " does not vanish at node " << j << "(" << dofValue <<")\n";
                    }
                  }
                }
              }
            }

            //check that fun values are consistent on common edges dofs
            auto hostBasisCoeffsLI = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), basisCoeffsLI);
            {
              bool areDifferent(false);
              auto numEdgeDOFs = basisPtr->getDofCount(1,0);

              for(ordinal_type j=0;j<numEdgeDOFs && !areDifferent;j++) {
                areDifferent = std::abs(hostBasisCoeffsLI(0,basisPtr->getDofOrdinal(1,edgeIndex[0],j))
                    - hostBasisCoeffsLI(1,basisPtr->getDofOrdinal(1,edgeIndex[1],j))) > 10*tol;
              }

              if(areDifferent) {
                errorFlag++;
                *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                *outStream << "Function DOFs on the common edge computed using Quad 0 basis functions are not consistent with those computed using Quad 1\n";
                *outStream << "Function DOFs for Quad 0 are:";
                for(ordinal_type j=0;j<numEdgeDOFs;j++)
                  *outStream << " " << hostBasisCoeffsLI(0,basisPtr->getDofOrdinal(1,edgeIndex[0],j));
                *outStream << "\nFunction DOFs for Quad 1 are:";
                for(ordinal_type j=0;j<numEdgeDOFs;j++)
                  *outStream << " " << hostBasisCoeffsLI(1,basisPtr->getDofOrdinal(1,edgeIndex[1],j));
                *outStream << std::endl;
              }
            }

            //check that fun values at reference points coincide with those computed using basis functions
            DynRankView ConstructWithLabel(basisValuesAtDofCoordsOriented, numCells, basisCardinality, basisCardinality, dim);
            DynRankView ConstructWithLabel(transformedBasisValuesAtDofCoordsOriented, numCells, basisCardinality, basisCardinality, dim);
            DynRankView basisValuesAtDofCoordsCells("inValues", numCells, basisCardinality, basisCardinality, dim);

            for (ordinal_type ic = 0; ic < numCells; ++ic)
              basisPtr->getValues(Kokkos::subview(basisValuesAtDofCoordsCells, ic, Kokkos::ALL(), Kokkos::ALL(), Kokkos::ALL()), Kokkos::subview(dofCoordsOriented, ic, Kokkos::ALL(), Kokkos::ALL()));

            // modify basis values to account for orientations
            ots::modifyBasisByOrientation(basisValuesAtDofCoordsOriented,
                basisValuesAtDofCoordsCells,
                elemOrts,
                basisPtr);

            // transform basis values
            DynRankView ConstructWithLabel(jacobianAtDofCoords, numCells, basisCardinality, dim, dim);
            DynRankView ConstructWithLabel(jacobianAtDofCoords_inv, numCells, basisCardinality, dim, dim);
            ct::setJacobian(jacobianAtDofCoords, dofCoordsOriented, physVertexes, cellTopo);
            ct::setJacobianInv (jacobianAtDofCoords_inv, jacobianAtDofCoords);
            fst::HCURLtransformVALUE(transformedBasisValuesAtDofCoordsOriented,
                jacobianAtDofCoords_inv,
                basisValuesAtDofCoordsOriented);
            DynRankView ConstructWithLabel(funAtDofCoordsOriented, numCells, basisCardinality, dim);
            Kokkos::parallel_for(Kokkos::RangePolicy<ExecSpaceType>(0,numCells),
            KOKKOS_LAMBDA (const int &i) {
              for(ordinal_type j=0; j<basisCardinality; ++j)
                for(ordinal_type d=0; d<dim; ++d) {
                  for(ordinal_type k=0; k<basisCardinality; ++k)
                    funAtDofCoordsOriented(i,j,d) += basisCoeffsLI(i,k)*transformedBasisValuesAtDofCoordsOriented(i,k,j,d);
                }
            });

            auto hostFunAtDofCoordsOriented = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), funAtDofCoordsOriented);
            auto hostFunAtDofCoords = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), funAtDofCoords);
            for(ordinal_type i=0; i<numCells; ++i) {
              ValueType error=0;
              for(ordinal_type j=0; j<basisCardinality; ++j) {
                for(ordinal_type d=0; d<dim; ++d)
                  error = std::max(std::abs( hostFunAtDofCoords(i,j,d) - hostFunAtDofCoordsOriented(i,j,d)), error);
              }

              if(error>100*tol) {
                errorFlag++;
                *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                *outStream << "Function values at reference points differ from those computed using basis functions of Quad " << i << "\n";
                *outStream << "Function values at reference points are:\n";
                for(ordinal_type j=0; j<basisCardinality; ++j)
                  *outStream << " (" << hostFunAtDofCoords(i,j,0) << "," << hostFunAtDofCoords(i,j,1) << ", " << hostFunAtDofCoords(i,j,2) << ")";
                *outStream << "\nFunction values at reference points computed using basis functions are\n";
                for(ordinal_type j=0; j<basisCardinality; ++j)
                  *outStream << " (" << hostFunAtDofCoordsOriented(i,j,0) << "," << hostFunAtDofCoordsOriented(i,j,1) << ", " << hostFunAtDofCoordsOriented(i,j,2) << ")";
                *outStream << std::endl;
              }
            }


            //compute projection-based interpolation of the Lagrangian interpolation
            DynRankView ConstructWithLabel(basisCoeffsHCurl, numCells, basisCardinality);
            {
              ordinal_type targetCubDegree(basisPtr->getDegree()),targetDerivCubDegree(basisPtr->getDegree()-1);

              Experimental::ProjectionStruct<DeviceType,ValueType> projStruct;
              projStruct.createHCurlProjectionStruct(basisPtr, targetCubDegree, targetDerivCubDegree);

              ordinal_type numPoints = projStruct.getNumTargetEvalPoints(), numCurlPoints = projStruct.getNumTargetDerivEvalPoints();
              DynRankView ConstructWithLabel(evaluationPoints, numCells, numPoints, dim);
              DynRankView ConstructWithLabel(evaluationCurlPoints, numCells, numCurlPoints, dim);

              pts::getHCurlEvaluationPoints(evaluationPoints,
                  evaluationCurlPoints,
                  elemOrts,
                  basisPtr,
                  &projStruct);


              DynRankView ConstructWithLabel(targetAtEvalPoints, numCells, numPoints, dim);
              DynRankView ConstructWithLabel(targetCurlAtEvalPoints, numCells, numCurlPoints);

              DynRankView ConstructWithLabel(hcurlBasisAtEvaluationPoints, numCells, basisCardinality , numPoints, dim);
              DynRankView ConstructWithLabel(hcurlBasisAtEvaluationPointsNonOriented, numCells, basisCardinality , numPoints, dim);
              for(int ic=0; ic<numCells; ic++)
                basisPtr->getValues(Kokkos::subview(hcurlBasisAtEvaluationPointsNonOriented, ic, Kokkos::ALL(), Kokkos::ALL(), Kokkos::ALL()), Kokkos::subview(evaluationPoints, ic, Kokkos::ALL(), Kokkos::ALL()), OPERATOR_VALUE);
              ots::modifyBasisByOrientation(hcurlBasisAtEvaluationPoints,
                  hcurlBasisAtEvaluationPointsNonOriented,
                  elemOrts,
                  basisPtr);

              DynRankView ConstructWithLabel(curlOfHCurlBasisAtEvaluationPoints, numCells, basisCardinality , numCurlPoints);
              if(numCurlPoints>0) {
                DynRankView ConstructWithLabel(curlOfHCurlBasisAtEvaluationPointsNonOriented, numCells, basisCardinality , numCurlPoints);
                for(int ic=0; ic<numCells; ic++)
                  basisPtr->getValues(Kokkos::subview(curlOfHCurlBasisAtEvaluationPointsNonOriented, ic, Kokkos::ALL(), Kokkos::ALL(), Kokkos::ALL()), Kokkos::subview(evaluationCurlPoints, ic, Kokkos::ALL(), Kokkos::ALL()), OPERATOR_CURL);
                ots::modifyBasisByOrientation(curlOfHCurlBasisAtEvaluationPoints,
                    curlOfHCurlBasisAtEvaluationPointsNonOriented,
                    elemOrts,
                    basisPtr);
              }


              Kokkos::parallel_for(Kokkos::RangePolicy<ExecSpaceType>(0,numCells),
              KOKKOS_LAMBDA (const int &ic) {
                for(int i=0;i<numPoints;i++) {
                  for(int k=0;k<basisCardinality;k++)
                    for(int d=0;d<dim;d++)
                      targetAtEvalPoints(ic,i,d) += basisCoeffsLI(ic,k)*hcurlBasisAtEvaluationPoints(ic,k,i,d);
                }
                for(int i=0;i<numCurlPoints;i++) {
                  for(int k=0;k<basisCardinality;k++)
                    targetCurlAtEvalPoints(ic,i) += basisCoeffsLI(ic,k)*curlOfHCurlBasisAtEvaluationPoints(ic,k,i);//funHCurlCoeffs(k)
                }
              });

              pts::getHCurlBasisCoeffs(basisCoeffsHCurl,
                  targetAtEvalPoints,
                  targetCurlAtEvalPoints,
                  evaluationPoints,
                  evaluationCurlPoints,
                  elemOrts,
                  basisPtr,
                  &projStruct);
            }

            //check that the basis coefficients of the Lagrangian interpolation are the same as those of the projection-based interpolation
            {
              ValueType diffErr(0);
              auto hostBasisCoeffsHCurl = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), basisCoeffsHCurl);
              for(int k=0;k<basisCardinality;k++) {
                for(int ic=0; ic<numCells; ic++)
                  diffErr = std::max(diffErr, std::abs(hostBasisCoeffsLI(ic,k) - hostBasisCoeffsHCurl(ic,k)));
              }

              if(diffErr > pow(7, degree-1)*tol) { //heuristic relation on how round-off error depends on degree
                errorFlag++;
                *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                *outStream << "HCURL_I" << degree << ": The weights recovered with the optimization are different than the one used for generating the functon."<<
                    "\nThe max The infinite norm of the difference between the weights is: " <<  diffErr << std::endl;
              }
            }

            //compute L2 projection of the Lagrangian interpolation
            DynRankView ConstructWithLabel(basisCoeffsL2, numCells, basisCardinality);
            {
              ordinal_type targetCubDegree(basisPtr->getDegree());

              Experimental::ProjectionStruct<DeviceType,ValueType> projStruct;
              projStruct.createL2ProjectionStruct(basisPtr, targetCubDegree);

              ordinal_type numPoints = projStruct.getNumTargetEvalPoints();
              DynRankView ConstructWithLabel(evaluationPoints, numCells, numPoints, dim);

              pts::getL2EvaluationPoints(evaluationPoints,
                  elemOrts,
                  basisPtr,
                  &projStruct);


              DynRankView ConstructWithLabel(targetAtEvalPoints, numCells, numPoints, dim);

              DynRankView ConstructWithLabel(hcurlBasisAtEvaluationPoints, numCells, basisCardinality , numPoints, dim);
              DynRankView ConstructWithLabel(hcurlBasisAtEvaluationPointsNonOriented, numCells, basisCardinality , numPoints, dim);
              for(int ic=0; ic<numCells; ic++)
                basisPtr->getValues(Kokkos::subview(hcurlBasisAtEvaluationPointsNonOriented, ic, Kokkos::ALL(), Kokkos::ALL(), Kokkos::ALL()), Kokkos::subview(evaluationPoints, ic, Kokkos::ALL(), Kokkos::ALL()), OPERATOR_VALUE);
              ots::modifyBasisByOrientation(hcurlBasisAtEvaluationPoints,
                  hcurlBasisAtEvaluationPointsNonOriented,
                  elemOrts,
                  basisPtr);

              Kokkos::parallel_for(Kokkos::RangePolicy<ExecSpaceType>(0,numCells),
              KOKKOS_LAMBDA (const int &ic) {
                for(int i=0;i<numPoints;i++) {
                  for(int k=0;k<basisCardinality;k++)
                    for(int d=0;d<dim;d++)
                      targetAtEvalPoints(ic,i,d) += basisCoeffsLI(ic,k)*hcurlBasisAtEvaluationPoints(ic,k,i,d);
                }
              });

              pts::getL2BasisCoeffs(basisCoeffsL2,
                  targetAtEvalPoints,
                  evaluationPoints,
                  elemOrts,
                  basisPtr,
                  &projStruct);
            }

            //check that the basis coefficients of the Lagrangian interpolation are the same as those of the L2 projection
            {
              ValueType diffErr = 0;
              auto hostBasisCoeffsL2 = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), basisCoeffsL2);
              for(int k=0;k<basisCardinality;k++) {
                for(int ic=0; ic<numCells; ic++)
                  diffErr = std::max(diffErr, std::abs(hostBasisCoeffsLI(ic,k) - hostBasisCoeffsL2(ic,k)));
              }

              if(diffErr > pow(7, degree-1)*tol) { //heuristic relation on how round-off error depends on degree
                errorFlag++;
                *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                *outStream << "HCURL_I" << degree << ": The weights recovered with the L2 optimization are different than the one used for generating the functon."<<
                    "\nThe max The infinite norm of the difference between the weights is: " <<  diffErr << std::endl;
              }
            }

            //compute Broken L2 projection of the Lagrangian interpolation
            DynRankView ConstructWithLabel(basisCoeffsL2DG, numCells, basisCardinality);
            {
              ordinal_type targetCubDegree(basisPtr->getDegree());

              Experimental::ProjectionStruct<DeviceType,ValueType> projStruct;
              projStruct.createL2DGProjectionStruct(basisPtr, targetCubDegree);

              ordinal_type numPoints = projStruct.getNumTargetEvalPoints();
              DynRankView ConstructWithLabel(evaluationPoints, numCells, numPoints, dim);

              pts::getL2DGEvaluationPoints(evaluationPoints,
                  basisPtr,
                  &projStruct);


              DynRankView ConstructWithLabel(targetAtEvalPoints, numCells, numPoints, dim);

              DynRankView ConstructWithLabel(hcurlBasisAtEvaluationPoints, numCells, basisCardinality , numPoints, dim);
              DynRankView ConstructWithLabel(hcurlBasisAtEvaluationPointsNonOriented, numCells, basisCardinality , numPoints, dim);
              for(int ic=0; ic<numCells; ic++)
                basisPtr->getValues(Kokkos::subview(hcurlBasisAtEvaluationPointsNonOriented, ic, Kokkos::ALL(), Kokkos::ALL(), Kokkos::ALL()), Kokkos::subview(evaluationPoints, ic, Kokkos::ALL(), Kokkos::ALL()), OPERATOR_VALUE);
              ots::modifyBasisByOrientation(hcurlBasisAtEvaluationPoints,
                  hcurlBasisAtEvaluationPointsNonOriented,
                  elemOrts,
                  basisPtr);

              Kokkos::parallel_for(Kokkos::RangePolicy<ExecSpaceType>(0,numCells),
              KOKKOS_LAMBDA (const int &ic) {
                for(int i=0;i<numPoints;i++) {
                  for(int k=0;k<basisCardinality;k++)
                    for(int d=0;d<dim;d++)
                      targetAtEvalPoints(ic,i,d) += basisCoeffsLI(ic,k)*hcurlBasisAtEvaluationPoints(ic,k,i,d);
                }
              });

              pts::getL2DGBasisCoeffs(basisCoeffsL2DG,
                  targetAtEvalPoints,
                  elemOrts,
                  basisPtr,
                  &projStruct);
            }

            //check that the basis coefficients of the Lagrangian interpolation are the same as those of the L2 projection
            {
              ValueType diffErr = 0;
              auto hostBasisCoeffsL2DG = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), basisCoeffsL2DG);
              for(int k=0;k<basisCardinality;k++) {
                for(int ic=0; ic<numCells; ic++)
                  diffErr = std::max(diffErr, std::abs(hostBasisCoeffsLI(ic,k) - hostBasisCoeffsL2DG(ic,k)));
              }

              if(diffErr > pow(7, degree-1)*tol) { //heuristic relation on how round-off error depends on degree
                errorFlag++;
                *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                *outStream << "HCURL_I" << degree << ": The weights recovered with the L2DG optimization are different than the one used for generating the functon."<<
                    "\nThe max The infinite norm of the difference between the weights is: " <<  diffErr << std::endl;
              }
            }
            delete basisPtr;
          }
        }
      }
    } while(std::next_permutation(&reorder[0]+1, &reorder[0]+4)); //reorder vertices of common face

  } catch (std::exception &err) {
    std::cout << " Exeption\n";
    *outStream << err.what() << "\n\n";
    errorFlag = -1000;
  }



  *outStream
  << "===============================================================================\n"
  << "|                                                                             |\n"
  << "|                 Test 3 (Orientation - HDIV)                                 |\n"
  << "|                                                                             |\n"
  << "===============================================================================\n";


  try {

    ordinal_type reorder[numTotalVertexes] = {0,1,2,3,4,5};

    do {
      ordinal_type orderback[numTotalVertexes];
      for(ordinal_type i=0;i<numTotalVertexes;++i) {
        orderback[reorder[i]]=i;
      }
      ValueType vertices[numTotalVertexes][dim];
      ordinal_type cells[numCells][numElemVertexes];
      std::copy(&cells_orig[0][0], &cells_orig[0][0]+numCells*numElemVertexes, &cells_rotated[0][0]);
      for (ordinal_type shift=0; shift<4; ++shift) {
        std::rotate_copy(&cells_orig[0][0], &cells_orig[0][0]+shift, &cells_orig[0][0]+4, &cells_rotated[0][0]);
        for(ordinal_type i=0; i<numCells;++i)
          for(ordinal_type j=0; j<numElemVertexes;++j)
            cells[i][j] = reorder[cells_rotated[i][j]];

        for(ordinal_type i=0; i<numTotalVertexes;++i)
          for(ordinal_type d=0; d<dim;++d)
            vertices[i][d] = vertices_orig[orderback[i]][d];

        *outStream <<  "Considering Quad 0: [ ";
        for(ordinal_type j=0; j<numElemVertexes;++j)
          *outStream << cells[0][j] << " ";
        *outStream << "] and Quad 1: [ ";
        for(ordinal_type j=0; j<numElemVertexes;++j)
          *outStream << cells[1][j] << " ";
        *outStream << "]\n";

        //computing vertices coords
        DynRankView ConstructWithLabel(physVertexes, numCells, numNodesPerElem, dim);
        auto hostPhysVertexes = Kokkos::create_mirror_view(physVertexes);
        for(ordinal_type i=0; i<numCells; ++i)
          for(ordinal_type j=0; j<numNodesPerElem; ++j)
            for(ordinal_type k=0; k<dim; ++k)
              hostPhysVertexes(i,j,k) = vertices[cells[i][j]][k];
        deep_copy(physVertexes, hostPhysVertexes);

        //computing common and edge
        ordinal_type edgeIndex[numCells];
        {
          edgeType edge={};
          for(ordinal_type i=0; i<numCells; ++i) {
            //compute common edge
            for (std::size_t ie=0; ie<cellTopo.getSideCount(); ++ie) {
              for (std::size_t k=0; k<cellTopo.getNodeCount(1,ie); ++k)
                edge[k]= cells_rotated[i][cellTopo.getNodeMap(1,ie,k)];

              if(edge == common_edge) edgeIndex[i]=ie;
            }
          }
        }

        // compute orientations for cells (one time computation)
        DynRankViewIntHost elemNodesHost(&cells[0][0], numCells, numElemVertexes);
        auto elemNodes = Kokkos::create_mirror_view_and_copy(MemSpaceType(),elemNodesHost);
        Kokkos::DynRankView<Orientation,DeviceType> elemOrts("elemOrts", numCells);
        ots::getOrientation(elemOrts, elemNodes, cellTopo);

        for (ordinal_type degree=1; degree <= max_degree; degree++) {

          basis_set.clear();
          if(degree==1)
            basis_set.push_back(new Basis_HDIV_QUAD_I1_FEM<DeviceType,ValueType,ValueType>());
          basis_set.push_back(new typename  CG_NBasis::HDIV_QUAD(degree,POINTTYPE_WARPBLEND));
          basis_set.push_back(new typename  CG_DNBasis::HDIV_QUAD(degree,POINTTYPE_EQUISPACED));

          for (auto basisPtr:basis_set) {

            auto name = basisPtr->getName();
            *outStream << " " << name << ": "<< degree << std::endl;
            ordinal_type basisCardinality = basisPtr->getCardinality();

            //compute DofCoords Oriented

            DynRankView ConstructWithLabel(dofCoordsOriented, numCells, basisCardinality, dim);
            DynRankView ConstructWithLabel(dofCoeffs, numCells, basisCardinality, dim);
            DynRankView ConstructWithLabel(physDofCoords, numCells, basisCardinality, dim);
            DynRankView ConstructWithLabel(funAtDofCoords, numCells, basisCardinality, dim);
            DynRankView ConstructWithLabel(basisCoeffsLI, numCells, basisCardinality);

            //compute Lagrangian Interpolation of fun
            {

              li::getDofCoordsAndCoeffs(dofCoordsOriented,  dofCoeffs, basisPtr, elemOrts);

              //need to transform dofCoeff to physical space (they transform as normals)
              DynRankView ConstructWithLabel(jacobian, numCells, basisCardinality, dim, dim);
              DynRankView ConstructWithLabel(jacobian_inv, numCells, basisCardinality, dim, dim);
              DynRankView ConstructWithLabel(jacobian_det, numCells, basisCardinality);
              ct::setJacobian(jacobian, dofCoordsOriented, physVertexes, cellTopo);
              ct::setJacobianInv (jacobian_inv, jacobian);
              ct::setJacobianDet (jacobian_det, jacobian);
              
              //Compute physical Dof Coordinates
              DynRankView ConstructWithLabel(linearBasisValuesAtDofCoord, numCells, numNodesPerElem);
              DynRankView ConstructWithLabel(fwdFunAtDofCoords, numCells, basisCardinality, dim);
              Kokkos::parallel_for(Kokkos::RangePolicy<ExecSpaceType>(0,numCells),
              KOKKOS_LAMBDA (const int &i) {
                FunDiv fun;
                auto basisValuesAtEvalDofCoord = Kokkos::subview(linearBasisValuesAtDofCoord,i,Kokkos::ALL());
                for(ordinal_type j=0; j<basisCardinality; ++j){
                  auto evalPoint = Kokkos::subview(dofCoordsOriented,i,j,Kokkos::ALL());
                  Impl::Basis_HGRAD_QUAD_C1_FEM::template Serial<OPERATOR_VALUE>::getValues(basisValuesAtEvalDofCoord, evalPoint);
                  for(ordinal_type k=0; k<numNodesPerElem; ++k)
                    for(ordinal_type d=0; d<dim; ++d)
                      physDofCoords(i,j,d) += physVertexes(i,k,d)*basisValuesAtEvalDofCoord(k);

                  for(ordinal_type k=0; k<dim; ++k)
                    funAtDofCoords(i,j,k) = fun(degree, physDofCoords(i,j,0), physDofCoords(i,j,1), k);
                  for(ordinal_type k=0; k<dim; ++k)
                    for(ordinal_type d=0; d<dim; ++d)
                      fwdFunAtDofCoords(i,j,k) += jacobian_det(i,j)*jacobian_inv(i,j,k,d)*funAtDofCoords(i,j,d);
                }
              });

              li::getBasisCoeffs(basisCoeffsLI, fwdFunAtDofCoords, dofCoeffs);
            }

            //Testing Kronecker property of basis functions
            {
              for(ordinal_type i=0; i<numCells; ++i) {
                DynRankView ConstructWithLabel(basisValuesAtDofCoords, numCells, basisCardinality, basisCardinality, dim);
                DynRankView ConstructWithLabel(basisValuesAtDofCoordsOriented, numCells, basisCardinality, basisCardinality, dim);
                auto inView = Kokkos::subview( dofCoordsOriented,i,Kokkos::ALL(),Kokkos::ALL());
                auto outView =Kokkos::subview( basisValuesAtDofCoords,i,Kokkos::ALL(),Kokkos::ALL(),Kokkos::ALL());
                basisPtr->getValues(outView, inView);

                // modify basis values to account for orientations
                ots::modifyBasisByOrientation(basisValuesAtDofCoordsOriented,
                    basisValuesAtDofCoords,
                    elemOrts,
                    basisPtr);

                DynRankView ConstructWithLabel(jacobian, numCells, basisCardinality, dim, dim);
                DynRankView ConstructWithLabel(jacobian_det, numCells, basisCardinality);
                ct::setJacobian(jacobian, dofCoordsOriented, physVertexes, cellTopo);
                ct::setJacobianDet (jacobian_det, jacobian);

                auto hostBasisValues = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), basisValuesAtDofCoordsOriented);
                auto hostDofCoeffs = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), dofCoeffs);
                for(ordinal_type k=0; k<basisCardinality; ++k) {
                  for(ordinal_type j=0; j<basisCardinality; ++j){
                    ValueType dofValue=0;
                    for(ordinal_type d=0; d<dim; ++d)
                      dofValue += hostBasisValues(i,k,j,d) * hostDofCoeffs(i,j,d);
                    if ( k==j && std::abs( dofValue - 1.0 ) > 100*tol ) {
                      errorFlag++;
                      *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                      *outStream << " Basis function " << k << " of cell " << i << " does not have unit value at its node (" << dofValue <<")\n";
                    }
                    if ( k!=j && std::abs( dofValue ) > tol ) {
                      errorFlag++;
                      *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                      *outStream << " Basis function " << k << " of cell " << i << " does not vanish at node " << j << "(" << dofValue <<")\n";
                    }
                  }
                }
              }
            }

            //check that fun values are consistent on common edge dofs
            auto hostBasisCoeffsLI = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), basisCoeffsLI);
            {
              bool areDifferent(false);
              auto numEdgeDOFs = basisPtr->getDofCount(dim-1,0);
              for(ordinal_type j=0;j<numEdgeDOFs && !areDifferent;j++) {
                areDifferent = std::abs(hostBasisCoeffsLI(0,basisPtr->getDofOrdinal(dim-1,edgeIndex[0],j))
                    - hostBasisCoeffsLI(1,basisPtr->getDofOrdinal(dim-1,edgeIndex[1],j))) > 10*tol;
              }

              if(areDifferent) {
                errorFlag++;
                auto hostPhysDofCoords = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), physDofCoords);
                *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                *outStream << "Function DOFs on common edge computed using Quad 0 basis functions are not consistent with those computed using Quad 1\n";
                *outStream << "Function DOFs for Quad 0 are:";
                for(ordinal_type j=0;j<numEdgeDOFs;j++)
                  *outStream << " " << hostBasisCoeffsLI(0,basisPtr->getDofOrdinal(dim-1,edgeIndex[0],j)) << " | (" << hostPhysDofCoords(0,basisPtr->getDofOrdinal(dim-1,edgeIndex[0],j),0) << "," << hostPhysDofCoords(0,basisPtr->getDofOrdinal(dim-1,edgeIndex[0],j),1) << ") ||";
                *outStream << "\nFunction DOFs for Quad 1 are:";
                for(ordinal_type j=0;j<numEdgeDOFs;j++)
                  *outStream << " " << hostBasisCoeffsLI(1,basisPtr->getDofOrdinal(dim-1,edgeIndex[1],j))<< " | (" << hostPhysDofCoords(1,basisPtr->getDofOrdinal(dim-1,edgeIndex[1],j),0) << "," << hostPhysDofCoords(1,basisPtr->getDofOrdinal(dim-1,edgeIndex[1],j),1)  << ") ||";
                *outStream << std::endl;
              }
            }

            //check that fun values at reference points coincide with those computed using basis functions
            DynRankView ConstructWithLabel(basisValuesAtDofCoordsOriented, numCells, basisCardinality, basisCardinality, dim);
            DynRankView ConstructWithLabel(transformedBasisValuesAtDofCoordsOriented, numCells, basisCardinality, basisCardinality, dim);
            DynRankView basisValuesAtDofCoordsCells("inValues", numCells, basisCardinality, basisCardinality, dim);

            for (ordinal_type ic = 0; ic < numCells; ++ic)
              basisPtr->getValues(Kokkos::subview(basisValuesAtDofCoordsCells, ic, Kokkos::ALL(), Kokkos::ALL(), Kokkos::ALL()), Kokkos::subview(dofCoordsOriented, ic, Kokkos::ALL(), Kokkos::ALL()));

            // modify basis values to account for orientations
            ots::modifyBasisByOrientation(basisValuesAtDofCoordsOriented,
                basisValuesAtDofCoordsCells,
                elemOrts,
                basisPtr);

            // transform basis values
            DynRankView ConstructWithLabel(jacobianAtDofCoords, numCells, basisCardinality, dim, dim);
            DynRankView ConstructWithLabel(jacobianAtDofCoords_det, numCells, basisCardinality);
            ct::setJacobian(jacobianAtDofCoords, dofCoordsOriented, physVertexes, cellTopo);
            ct::setJacobianDet (jacobianAtDofCoords_det, jacobianAtDofCoords);
            fst::HDIVtransformVALUE(transformedBasisValuesAtDofCoordsOriented,
                jacobianAtDofCoords,
                jacobianAtDofCoords_det,
                basisValuesAtDofCoordsOriented);
            DynRankView ConstructWithLabel(funAtDofCoordsOriented, numCells, basisCardinality, dim);
            Kokkos::parallel_for(Kokkos::RangePolicy<ExecSpaceType>(0,numCells),
            KOKKOS_LAMBDA (const int &i) {
              for(ordinal_type j=0; j<basisCardinality; ++j) {
                for(ordinal_type d=0; d<dim; ++d)
                  for(ordinal_type k=0; k<basisCardinality; ++k)
                    funAtDofCoordsOriented(i,j,d) += basisCoeffsLI(i,k)*transformedBasisValuesAtDofCoordsOriented(i,k,j,d);
              }
            });

            auto hostFunAtDofCoordsOriented = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), funAtDofCoordsOriented);
            auto hostFunAtDofCoords = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), funAtDofCoords);
            for(ordinal_type i=0; i<numCells; ++i) {
              ValueType error=0;
              for(ordinal_type j=0; j<basisCardinality; ++j) {
                for(ordinal_type d=0; d<dim; ++d)
                  error = std::max(std::abs( hostFunAtDofCoords(i,j,d) - hostFunAtDofCoordsOriented(i,j,d)), error);
              }

              if(error>100*tol) {
                errorFlag++;
                *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                *outStream << "Function values at reference points differ from those computed using basis functions of Quad " << i << "\n";
                *outStream << "Function values at reference points are:\n";
                for(ordinal_type j=0; j<basisCardinality; ++j)
                  *outStream << " (" << hostFunAtDofCoords(i,j,0) << "," << hostFunAtDofCoords(i,j,1) << ")";
                *outStream << "\nFunction values at reference points computed using basis functions are\n";
                for(ordinal_type j=0; j<basisCardinality; ++j)
                  *outStream << " (" << hostFunAtDofCoordsOriented(i,j,0) << "," << hostFunAtDofCoordsOriented(i,j,1) << ")";
                *outStream << std::endl;
              }
            }

            //compute projection-based interpolation of the Lagrangian interpolation
            DynRankView ConstructWithLabel(basisCoeffsHDiv, numCells, basisCardinality);
            {
              ordinal_type targetCubDegree(basisPtr->getDegree()),targetDerivCubDegree(basisPtr->getDegree()-1);

              Experimental::ProjectionStruct<DeviceType,ValueType> projStruct;
              projStruct.createHDivProjectionStruct(basisPtr, targetCubDegree, targetDerivCubDegree);

              ordinal_type numPoints = projStruct.getNumTargetEvalPoints(), numDivPoints = projStruct.getNumTargetDerivEvalPoints();

              DynRankView ConstructWithLabel(evaluationPoints, numCells, numPoints, dim);
              DynRankView ConstructWithLabel(evaluationDivPoints, numCells, numDivPoints, dim);

              pts::getHDivEvaluationPoints(evaluationPoints,
                  evaluationDivPoints,
                  elemOrts,
                  basisPtr,
                  &projStruct);

              DynRankView ConstructWithLabel(targetAtEvalPoints, numCells, numPoints, dim);
              DynRankView ConstructWithLabel(targetDivAtEvalPoints, numCells, numDivPoints);

              DynRankView ConstructWithLabel(hdivBasisAtEvaluationPoints, numCells, basisCardinality , numPoints, dim);
              DynRankView ConstructWithLabel(hdivBasisAtEvaluationPointsNonOriented, numCells, basisCardinality , numPoints, dim);
              for(ordinal_type ic=0; ic<numCells; ++ic)
                basisPtr->getValues(Kokkos::subview(hdivBasisAtEvaluationPointsNonOriented, ic, Kokkos::ALL(), Kokkos::ALL(), Kokkos::ALL()), Kokkos::subview(evaluationPoints, ic, Kokkos::ALL(), Kokkos::ALL()), OPERATOR_VALUE);
              ots::modifyBasisByOrientation(hdivBasisAtEvaluationPoints,
                  hdivBasisAtEvaluationPointsNonOriented,
                  elemOrts,
                  basisPtr);

              DynRankView ConstructWithLabel(divOfHDivBasisAtEvaluationPoints, numCells, basisCardinality , numDivPoints);
              if(numDivPoints>0) {
                DynRankView ConstructWithLabel(divOfHDivBasisAtEvaluationPointsNonOriented, numCells, basisCardinality , numDivPoints);
                for(ordinal_type ic=0; ic<numCells; ++ic)
                  basisPtr->getValues(Kokkos::subview(divOfHDivBasisAtEvaluationPointsNonOriented, ic, Kokkos::ALL(), Kokkos::ALL()), Kokkos::subview(evaluationDivPoints, ic, Kokkos::ALL(), Kokkos::ALL()), OPERATOR_DIV);
                ots::modifyBasisByOrientation(divOfHDivBasisAtEvaluationPoints,
                    divOfHDivBasisAtEvaluationPointsNonOriented,
                    elemOrts,
                    basisPtr);
              }



              Kokkos::parallel_for(Kokkos::RangePolicy<ExecSpaceType>(0,numCells),
              KOKKOS_LAMBDA (const int &ic) {
                for(int i=0;i<numPoints;i++) {
                  for(int k=0;k<basisCardinality;k++)
                    for(int d=0;d<dim;d++)
                      targetAtEvalPoints(ic,i,d) += basisCoeffsLI(ic,k)*hdivBasisAtEvaluationPoints(ic,k,i,d);
                }
                for(int i=0;i<numDivPoints;i++) {
                  for(int k=0;k<basisCardinality;k++)
                    targetDivAtEvalPoints(ic,i) += basisCoeffsLI(ic,k)*divOfHDivBasisAtEvaluationPoints(ic,k,i);//basisCoeffsLI(k)
                }
              });

              pts::getHDivBasisCoeffs(basisCoeffsHDiv,
                  targetAtEvalPoints,
                  targetDivAtEvalPoints,
                  evaluationPoints,
                  evaluationDivPoints,
                  elemOrts,
                  basisPtr,
                  &projStruct);
            }

            //check that the basis coefficients of the Lagrangian interpolation are the same as those of the projection-based interpolation
            {
              ValueType diffErr(0);
              auto hostBasisCoeffsHDiv = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), basisCoeffsHDiv);
              for(int k=0;k<basisCardinality;k++) {
                //std::cout << "["<< basisCoeffsLI(0,k) << " " <<  basisCoeffsHDiv(0,k) << "] [" << basisCoeffsLI(1,k) << " " <<  basisCoeffsHDiv(1,k) << "]" <<std::endl;
                for(ordinal_type ic=0; ic<numCells; ++ic)
                  diffErr = std::max(diffErr, std::abs(hostBasisCoeffsLI(ic,k) - hostBasisCoeffsHDiv(ic,k)));
              }

              if(diffErr > pow(7, degree-1)*tol) { //heuristic relation on how round-off error depends on degree
                errorFlag++;
                *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                *outStream << "HDIV_I" << degree << ": The weights recovered with the optimization are different than the one used for generating the functon."<<
                    "\nThe max The infinite norm of the difference between the weights is: " <<  diffErr << std::endl;
              }
            }


            //compute L2 projection interpolation of the Lagrangian interpolation
            DynRankView ConstructWithLabel(basisCoeffsL2, numCells, basisCardinality);
            {
              ordinal_type targetCubDegree(basisPtr->getDegree());

              Experimental::ProjectionStruct<DeviceType,ValueType> projStruct;
              projStruct.createL2ProjectionStruct(basisPtr, targetCubDegree);

              ordinal_type numPoints = projStruct.getNumTargetEvalPoints();

              DynRankView ConstructWithLabel(evaluationPoints, numCells, numPoints, dim);

              pts::getL2EvaluationPoints(evaluationPoints,
                  elemOrts,
                  basisPtr,
                  &projStruct);

              DynRankView ConstructWithLabel(targetAtEvalPoints, numCells, numPoints, dim);

              DynRankView ConstructWithLabel(hdivBasisAtEvaluationPoints, numCells, basisCardinality , numPoints, dim);
              DynRankView ConstructWithLabel(hdivBasisAtEvaluationPointsNonOriented, numCells, basisCardinality , numPoints, dim);
              for(ordinal_type ic=0; ic<numCells; ++ic)
                basisPtr->getValues(Kokkos::subview(hdivBasisAtEvaluationPointsNonOriented, ic, Kokkos::ALL(), Kokkos::ALL(), Kokkos::ALL()), Kokkos::subview(evaluationPoints, ic, Kokkos::ALL(), Kokkos::ALL()), OPERATOR_VALUE);
              ots::modifyBasisByOrientation(hdivBasisAtEvaluationPoints,
                  hdivBasisAtEvaluationPointsNonOriented,
                  elemOrts,
                  basisPtr);

              Kokkos::parallel_for(Kokkos::RangePolicy<ExecSpaceType>(0,numCells),
              KOKKOS_LAMBDA (const int &ic) {
                for(int i=0;i<numPoints;i++) {
                  for(int k=0;k<basisCardinality;k++)
                    for(int d=0;d<dim;d++)
                      targetAtEvalPoints(ic,i,d) += basisCoeffsLI(ic,k)*hdivBasisAtEvaluationPoints(ic,k,i,d);
                }
              });

              pts::getL2BasisCoeffs(basisCoeffsL2,
                  targetAtEvalPoints,
                  evaluationPoints,
                  elemOrts,
                  basisPtr,
                  &projStruct);
            }

            //check that the basis coefficients of the Lagrangian interpolation are the same as those of the L2 projection
            {
              ValueType diffErr = 0;
              auto hostBasisCoeffsL2 = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), basisCoeffsL2);
              for(int k=0;k<basisCardinality;k++) {
                //std::cout << "["<< basisCoeffsLI(0,k) << " " <<  basisCoeffsHDiv(0,k) << "] [" << basisCoeffsLI(1,k) << " " <<  basisCoeffsHDiv(1,k) << "]" <<std::endl;
                for(ordinal_type ic=0; ic<numCells; ++ic)
                  diffErr = std::max(diffErr, std::abs(hostBasisCoeffsLI(ic,k) - hostBasisCoeffsL2(ic,k)));
              }

              if(diffErr > pow(7, degree-1)*tol) { //heuristic relation on how round-off error depends on degree
                errorFlag++;
                *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                *outStream << "HDIV_I" << degree << ": The weights recovered with the L2 optimization are different than the one used for generating the function."<<
                    "\nThe max The infinite norm of the difference between the weights is: " <<  diffErr << std::endl;
              }
            }

            //compute L2 Discontinuous projection interpolation of the Lagrangian interpolation
            DynRankView ConstructWithLabel(basisCoeffsL2DG, numCells, basisCardinality);
            {
              ordinal_type targetCubDegree(basisPtr->getDegree());

              Experimental::ProjectionStruct<DeviceType,ValueType> projStruct;
              projStruct.createL2DGProjectionStruct(basisPtr, targetCubDegree);

              ordinal_type numPoints = projStruct.getNumTargetEvalPoints();

              DynRankView ConstructWithLabel(evaluationPoints, numCells, numPoints, dim);

              pts::getL2DGEvaluationPoints(evaluationPoints,
                  basisPtr,
                  &projStruct);

              DynRankView ConstructWithLabel(targetAtEvalPoints, numCells, numPoints, dim);

              DynRankView ConstructWithLabel(hdivBasisAtEvaluationPoints, numCells, basisCardinality , numPoints, dim);
              DynRankView ConstructWithLabel(hdivBasisAtEvaluationPointsNonOriented, numCells, basisCardinality , numPoints, dim);
              for(ordinal_type ic=0; ic<numCells; ++ic)
                basisPtr->getValues(Kokkos::subview(hdivBasisAtEvaluationPointsNonOriented, ic, Kokkos::ALL(), Kokkos::ALL(), Kokkos::ALL()), Kokkos::subview(evaluationPoints, ic, Kokkos::ALL(), Kokkos::ALL()), OPERATOR_VALUE);
              ots::modifyBasisByOrientation(hdivBasisAtEvaluationPoints,
                  hdivBasisAtEvaluationPointsNonOriented,
                  elemOrts,
                  basisPtr);

              Kokkos::parallel_for(Kokkos::RangePolicy<ExecSpaceType>(0,numCells),
              KOKKOS_LAMBDA (const int &ic) {
                for(int i=0;i<numPoints;i++) {
                  for(int k=0;k<basisCardinality;k++)
                    for(int d=0;d<dim;d++)
                      targetAtEvalPoints(ic,i,d) += basisCoeffsLI(ic,k)*hdivBasisAtEvaluationPoints(ic,k,i,d);
                }
              });

              pts::getL2DGBasisCoeffs(basisCoeffsL2DG,
                  targetAtEvalPoints,
                  elemOrts,
                  basisPtr,
                  &projStruct);
            }

            //check that the basis coefficients of the Lagrangian interpolation are the same as those of the L2 projection
            {
              ValueType diffErr = 0;
              auto hostBasisCoeffsL2DG = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), basisCoeffsL2DG);
              for(int k=0;k<basisCardinality;k++) {
                //std::cout << "["<< basisCoeffsLI(0,k) << " " <<  basisCoeffsHDiv(0,k) << "] [" << basisCoeffsLI(1,k) << " " <<  basisCoeffsHDiv(1,k) << "]" <<std::endl;
                for(ordinal_type ic=0; ic<numCells; ++ic)
                  diffErr = std::max(diffErr, std::abs(hostBasisCoeffsLI(ic,k) - hostBasisCoeffsL2DG(ic,k)));
              }

              if(diffErr > pow(7, degree-1)*tol) { //heuristic relation on how round-off error depends on degree
                errorFlag++;
                *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                *outStream << "HDIV_I" << degree << ": The weights recovered with the L2DG optimization are different than the one used for generating the function."<<
                    "\nThe max The infinite norm of the difference between the weights is: " <<  diffErr << std::endl;
              }
            }


            delete basisPtr;
          }
        }
      }
    } while(std::next_permutation(&reorder[0]+1, &reorder[0]+4)); //reorder vertices of common face

  } catch (std::exception &err) {
    std::cout << " Exeption\n";
    *outStream << err.what() << "\n\n";
    errorFlag = -1000;
  }

  *outStream
  << "===============================================================================\n"
  << "|                                                                             |\n"
  << "|                 Test 4 (Orientation - HVOL)                                 |\n"
  << "|                                                                             |\n"
  << "===============================================================================\n";


  try {


    ValueType vertices[numTotalVertexes][dim];
    ordinal_type cells[numCells][numElemVertexes];

    for(ordinal_type i=0; i<numCells;++i)
      for(ordinal_type j=0; j<numElemVertexes;++j)
        cells[i][j] = cells_orig[i][j];

    for(ordinal_type i=0; i<numTotalVertexes;++i)
      for(ordinal_type d=0; d<dim;++d)
        vertices[i][d] = vertices_orig[i][d];

    *outStream <<  "Considering Quad 0: [ ";
    for(ordinal_type j=0; j<numElemVertexes;++j)
      *outStream << cells[0][j] << " ";
    *outStream << "] and Quad 1: [ ";
    for(ordinal_type j=0; j<numElemVertexes;++j)
      *outStream << cells[1][j] << " ";
    *outStream << "]\n";

    //computing vertices coords
    DynRankView ConstructWithLabel(physVertexes, numCells, numNodesPerElem, dim);
    auto hostPhysVertexes = Kokkos::create_mirror_view(physVertexes);
    for(ordinal_type i=0; i<numCells; ++i)
      for(ordinal_type j=0; j<numNodesPerElem; ++j)
        for(ordinal_type k=0; k<dim; ++k)
          hostPhysVertexes(i,j,k) = vertices[cells[i][j]][k];
    deep_copy(physVertexes, hostPhysVertexes);

    // compute orientations for cells (one time computation)
    DynRankViewIntHost elemNodesHost(&cells[0][0], numCells, numElemVertexes);
    auto elemNodes = Kokkos::create_mirror_view_and_copy(MemSpaceType(),elemNodesHost);
    Kokkos::DynRankView<Orientation,DeviceType> elemOrts("elemOrts", numCells);
    ots::getOrientation(elemOrts, elemNodes, cellTopo);

    for (ordinal_type degree=1; degree <= max_degree; degree++) {

      basis_set.clear();
      if(degree==1)
        basis_set.push_back(new Basis_HVOL_C0_FEM<DeviceType,ValueType,ValueType>(cellTopo));
      basis_set.push_back(new typename  CG_NBasis::HVOL_QUAD(degree,POINTTYPE_EQUISPACED));
      basis_set.push_back(new typename  CG_DNBasis::HVOL_QUAD(degree,POINTTYPE_WARPBLEND));

      for (auto basisPtr:basis_set) {

        auto name = basisPtr->getName();
        *outStream << " " << name <<  ": " << degree << std::endl;

        ordinal_type basisCardinality = basisPtr->getCardinality();

        //compute DofCoords Oriented
        DynRankView ConstructWithLabel(dofCoordsOriented, numCells, basisCardinality, dim);
        DynRankView ConstructWithLabel(dofCoeffsPhys, numCells, basisCardinality);
        DynRankView ConstructWithLabel(physDofCoords, numCells, basisCardinality, dim);
        DynRankView ConstructWithLabel(funAtDofCoords, numCells, basisCardinality);
        DynRankView ConstructWithLabel(basisCoeffsLI, numCells, basisCardinality);

        //compute Lagrangian Interpolation of fun
        {
          li::getDofCoordsAndCoeffs(dofCoordsOriented,  dofCoeffsPhys, basisPtr, elemOrts);

          //need to transform dofCoeff to physical space (they transform as normals)
          DynRankView ConstructWithLabel(jacobian, numCells, basisCardinality, dim, dim);
          DynRankView ConstructWithLabel(jacobian_det, numCells, basisCardinality);
          ct::setJacobian(jacobian, dofCoordsOriented, physVertexes, cellTopo);
          ct::setJacobianDet (jacobian_det, jacobian);

          //Compute physical Dof Coordinates
          DynRankView ConstructWithLabel(linearBasisValuesAtDofCoord, numCells, numNodesPerElem);
          DynRankView ConstructWithLabel(fwdFunAtDofCoords, numCells, basisCardinality);
          Kokkos::parallel_for(Kokkos::RangePolicy<ExecSpaceType>(0,numCells),
          KOKKOS_LAMBDA (const int &i) {
            Fun fun;
            auto basisValuesAtEvalDofCoord = Kokkos::subview(linearBasisValuesAtDofCoord,i,Kokkos::ALL());
            for(ordinal_type j=0; j<basisCardinality; ++j){
              auto evalPoint = Kokkos::subview(dofCoordsOriented,i,j,Kokkos::ALL());
              Impl::Basis_HGRAD_QUAD_C1_FEM::template Serial<OPERATOR_VALUE>::getValues(basisValuesAtEvalDofCoord, evalPoint);
              for(ordinal_type k=0; k<numNodesPerElem; ++k)
                for(ordinal_type d=0; d<dim; ++d)
                  physDofCoords(i,j,d) += physVertexes(i,k,d)*basisValuesAtEvalDofCoord(k);

              funAtDofCoords(i,j) = fun(degree, physDofCoords(i,j,0), physDofCoords(i,j,1));
              fwdFunAtDofCoords(i,j) = jacobian_det(i,j)*funAtDofCoords(i,j);
            }
          });

          li::getBasisCoeffs(basisCoeffsLI, fwdFunAtDofCoords, dofCoeffsPhys);
        }

        //Testing Kronecker property of basis functions
        {
          for(ordinal_type i=0; i<numCells; ++i) {
            DynRankView ConstructWithLabel(basisValuesAtDofCoords, numCells, basisCardinality, basisCardinality);
            DynRankView ConstructWithLabel(basisValuesAtDofCoordsOriented, numCells, basisCardinality, basisCardinality);
            auto inView = Kokkos::subview( dofCoordsOriented,i,Kokkos::ALL(),Kokkos::ALL());
            auto outView =Kokkos::subview( basisValuesAtDofCoords,i,Kokkos::ALL(),Kokkos::ALL());
            basisPtr->getValues(outView, inView);

            // modify basis values to account for orientations
            ots::modifyBasisByOrientation(basisValuesAtDofCoordsOriented,
                basisValuesAtDofCoords,
                elemOrts,
                basisPtr);

            auto hostBasisValues = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), basisValuesAtDofCoordsOriented);
            auto hostDofCoeffsPhys = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), dofCoeffsPhys);
            for(ordinal_type k=0; k<basisCardinality; ++k) {
              for(ordinal_type j=0; j<basisCardinality; ++j){
                ValueType dofValue = hostBasisValues(i,k,j) * hostDofCoeffsPhys(i,j);
                if ( k==j && std::abs( dofValue - 1.0 ) > 100*tol ) {
                  errorFlag++;
                  *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                  *outStream << " Basis function " << k << " of cell " << i << " does not have unit value at its node (" << dofValue <<")\n";
                }
                if ( k!=j && std::abs( dofValue ) > tol ) {
                  errorFlag++;
                  *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
                  *outStream << " Basis function " << k << " of cell " << i << " does not vanish at node " << j << "(" << dofValue <<")\n";
                }
              }
            }
          }
        }

        //check that fun values at reference points coincide with those computed using basis functions
        DynRankView ConstructWithLabel(basisValuesAtDofCoordsOriented, numCells, basisCardinality, basisCardinality);
        DynRankView ConstructWithLabel(transformedBasisValuesAtDofCoordsOriented, numCells, basisCardinality, basisCardinality);
        DynRankView basisValuesAtDofCoordsCells("inValues", numCells, basisCardinality, basisCardinality);

        for (ordinal_type ic = 0; ic < numCells; ++ic)
          basisPtr->getValues(Kokkos::subview(basisValuesAtDofCoordsCells, ic, Kokkos::ALL(), Kokkos::ALL()), Kokkos::subview(dofCoordsOriented, ic, Kokkos::ALL(), Kokkos::ALL()));

        // modify basis values to account for orientations
        ots::modifyBasisByOrientation(basisValuesAtDofCoordsOriented,
            basisValuesAtDofCoordsCells,
            elemOrts,
            basisPtr);

        // transform basis values
        DynRankView ConstructWithLabel(jacobian, numCells, basisCardinality, dim, dim);
        DynRankView ConstructWithLabel(jacobian_det, numCells, basisCardinality);
        ct::setJacobian(jacobian, dofCoordsOriented, physVertexes, cellTopo);
        ct::setJacobianDet (jacobian_det, jacobian);
        fst::HVOLtransformVALUE(transformedBasisValuesAtDofCoordsOriented,
            jacobian_det,
            basisValuesAtDofCoordsOriented);

        DynRankView ConstructWithLabel(funAtDofCoordsOriented, numCells, basisCardinality);
        Kokkos::parallel_for(Kokkos::RangePolicy<ExecSpaceType>(0,numCells),
        KOKKOS_LAMBDA (const int &i) {
          for(ordinal_type j=0; j<basisCardinality; ++j) {
            for(ordinal_type k=0; k<basisCardinality; ++k)
              funAtDofCoordsOriented(i,j) += basisCoeffsLI(i,k)*transformedBasisValuesAtDofCoordsOriented(i,k,j);
          }
        });

        auto hostFunAtDofCoordsOriented = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), funAtDofCoordsOriented);
        auto hostFunAtDofCoords = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), funAtDofCoords);
        for(ordinal_type i=0; i<numCells; ++i) {
          ValueType error=0;
          for(ordinal_type j=0; j<basisCardinality; ++j) {
            error = std::max(std::abs( hostFunAtDofCoords(i,j) - hostFunAtDofCoordsOriented(i,j)), error);
          }

          if(error>100*tol) {
            errorFlag++;
            *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
            *outStream << "Function values at reference points differ from those computed using basis functions of Quad " << i << "\n";
            *outStream << "Function values at reference points are:\n";
            for(ordinal_type j=0; j<basisCardinality; ++j)
              *outStream << " (" << hostFunAtDofCoords(i,j)  << ")";
            *outStream << "\nFunction values at reference points computed using basis functions are\n";
            for(ordinal_type j=0; j<basisCardinality; ++j)
              *outStream << " (" << hostFunAtDofCoordsOriented(i,j)  << ")";
            *outStream << std::endl;
          }
        }

        //compute projection-based interpolation of the Lagrangian interpolation
        DynRankView ConstructWithLabel(basisCoeffsHVol, numCells, basisCardinality);
        {
          ordinal_type targetCubDegree(basisPtr->getDegree());

          Experimental::ProjectionStruct<DeviceType,ValueType> projStruct;
          projStruct.createHVolProjectionStruct(basisPtr, targetCubDegree);

          ordinal_type numPoints = projStruct.getNumTargetEvalPoints();
          DynRankView ConstructWithLabel(evaluationPoints, numCells, numPoints, dim);


          pts::getHVolEvaluationPoints(evaluationPoints,
              elemOrts,
              basisPtr,
              &projStruct);


          DynRankView ConstructWithLabel(targetAtEvalPoints, numCells, numPoints);

          DynRankView ConstructWithLabel(hvolBasisAtEvaluationPoints, numCells, basisCardinality , numPoints);
          DynRankView ConstructWithLabel(hvolBasisAtEvaluationPointsNonOriented, numCells, basisCardinality , numPoints);
          for(int ic=0; ic<numCells; ic++)
            basisPtr->getValues(Kokkos::subview(hvolBasisAtEvaluationPointsNonOriented, ic, Kokkos::ALL(), Kokkos::ALL()), Kokkos::subview(evaluationPoints, ic, Kokkos::ALL(), Kokkos::ALL()), OPERATOR_VALUE);
          ots::modifyBasisByOrientation(hvolBasisAtEvaluationPoints,
              hvolBasisAtEvaluationPointsNonOriented,
              elemOrts,
              basisPtr);

          Kokkos::parallel_for(Kokkos::RangePolicy<ExecSpaceType>(0,numCells),
          KOKKOS_LAMBDA (const int &ic) {
            for(int i=0;i<numPoints;i++) {
              for(int k=0;k<basisCardinality;k++)
                targetAtEvalPoints(ic,i) += basisCoeffsLI(ic,k)*hvolBasisAtEvaluationPoints(ic,k,i);
            }
          });

          pts::getHVolBasisCoeffs(basisCoeffsHVol,
              targetAtEvalPoints,
              evaluationPoints,
              elemOrts,
              basisPtr,
              &projStruct);
        }

        //check that the basis coefficients of the Lagrangian interpolation are the same as those of the projection-based interpolation
        auto hostBasisCoeffsLI = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), basisCoeffsLI);
        {
          ValueType diffErr(0);
          auto hostBasisCoeffsHVol = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), basisCoeffsHVol);
          for(int k=0;k<basisCardinality;k++) {
            for(int ic=0; ic<numCells; ic++)
              diffErr = std::max(diffErr, std::abs(hostBasisCoeffsLI(ic,k) - hostBasisCoeffsHVol(ic,k)));
          }

          //Check that the two representations of the gradient of ifun are consistent
          if(diffErr > pow(16, degree)*tol) { //heuristic relation on how round-off error depends on degree
            errorFlag++;
            *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
            *outStream << "HVOL_C" << degree << ": The weights recovered with the optimization are different than the one used for generating the functon."<<
                "\nThe max The infinite norm of the difference between the weights is: " <<  diffErr << std::endl;
          }
        }

        //compute L2 projection of the Lagrangian interpolation
        DynRankView ConstructWithLabel(basisCoeffsL2, numCells, basisCardinality);
        {
          ordinal_type targetCubDegree(basisPtr->getDegree());

          Experimental::ProjectionStruct<DeviceType,ValueType> projStruct;
          projStruct.createL2ProjectionStruct(basisPtr, targetCubDegree);

          ordinal_type numPoints = projStruct.getNumTargetEvalPoints();
          DynRankView ConstructWithLabel(evaluationPoints, numCells, numPoints, dim);


          pts::getL2EvaluationPoints(evaluationPoints,
              elemOrts,
              basisPtr,
              &projStruct);


          DynRankView ConstructWithLabel(targetAtEvalPoints, numCells, numPoints);

          DynRankView ConstructWithLabel(hvolBasisAtEvaluationPoints, numCells, basisCardinality , numPoints);
          DynRankView ConstructWithLabel(hvolBasisAtEvaluationPointsNonOriented, numCells, basisCardinality , numPoints);
          for(int ic=0; ic<numCells; ic++)
            basisPtr->getValues(Kokkos::subview(hvolBasisAtEvaluationPointsNonOriented, ic, Kokkos::ALL(), Kokkos::ALL()), Kokkos::subview(evaluationPoints, ic, Kokkos::ALL(), Kokkos::ALL()), OPERATOR_VALUE);
          ots::modifyBasisByOrientation(hvolBasisAtEvaluationPoints,
              hvolBasisAtEvaluationPointsNonOriented,
              elemOrts,
              basisPtr);

          Kokkos::parallel_for(Kokkos::RangePolicy<ExecSpaceType>(0,numCells),
          KOKKOS_LAMBDA (const int &ic) {
            for(int i=0;i<numPoints;i++) {
              for(int k=0;k<basisCardinality;k++)
                targetAtEvalPoints(ic,i) += basisCoeffsLI(ic,k)*hvolBasisAtEvaluationPoints(ic,k,i);
            }
          });

          pts::getL2BasisCoeffs(basisCoeffsL2,
              targetAtEvalPoints,
              evaluationPoints,
              elemOrts,
              basisPtr,
              &projStruct);
        }

        //check that the basis coefficients of the Lagrangian interpolation are the same as those of the L2 projection
        {
          ValueType diffErr = 0;
          auto hostBasisCoeffsL2 = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), basisCoeffsL2);
          for(int k=0;k<basisCardinality;k++) {
            for(int ic=0; ic<numCells; ic++)
              diffErr = std::max(diffErr, std::abs(hostBasisCoeffsLI(ic,k) - hostBasisCoeffsL2(ic,k)));
          }

          if(diffErr > pow(16, degree)*tol) { //heuristic relation on how round-off error depends on degree
            errorFlag++;
            *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
            *outStream << "HVOL_C" << degree << ": The weights recovered with the L2 optimization are different than the one used for generating the functon."<<
                "\nThe max The infinite norm of the difference between the weights is: " <<  diffErr << std::endl;
          }
        }

        //compute L2DG projection of the Lagrangian interpolation
        DynRankView ConstructWithLabel(basisCoeffsL2DG, numCells, basisCardinality);
        {
          ordinal_type targetCubDegree(basisPtr->getDegree());

          Experimental::ProjectionStruct<DeviceType,ValueType> projStruct;
          projStruct.createL2DGProjectionStruct(basisPtr, targetCubDegree);

          ordinal_type numPoints = projStruct.getNumTargetEvalPoints();
          DynRankView ConstructWithLabel(evaluationPoints, numCells, numPoints, dim);


          pts::getL2DGEvaluationPoints(evaluationPoints,
              basisPtr,
              &projStruct);


          DynRankView ConstructWithLabel(targetAtEvalPoints, numCells, numPoints);

          DynRankView ConstructWithLabel(hvolBasisAtEvaluationPoints, numCells, basisCardinality , numPoints);
          DynRankView ConstructWithLabel(hvolBasisAtEvaluationPointsNonOriented, numCells, basisCardinality , numPoints);
          for(int ic=0; ic<numCells; ic++)
            basisPtr->getValues(Kokkos::subview(hvolBasisAtEvaluationPointsNonOriented, ic, Kokkos::ALL(), Kokkos::ALL()), Kokkos::subview(evaluationPoints, ic, Kokkos::ALL(), Kokkos::ALL()), OPERATOR_VALUE);
          ots::modifyBasisByOrientation(hvolBasisAtEvaluationPoints,
              hvolBasisAtEvaluationPointsNonOriented,
              elemOrts,
              basisPtr);

          Kokkos::parallel_for(Kokkos::RangePolicy<ExecSpaceType>(0,numCells),
          KOKKOS_LAMBDA (const int &ic) {

            for(int i=0;i<numPoints;i++) {
              for(int k=0;k<basisCardinality;k++)
                targetAtEvalPoints(ic,i) += basisCoeffsLI(ic,k)*hvolBasisAtEvaluationPoints(ic,k,i);
            }
          });

          pts::getL2DGBasisCoeffs(basisCoeffsL2DG,
              targetAtEvalPoints,
              //evaluationPoints,
              //elemOrts,
              basisPtr,
              &projStruct);
        }

        //check that the basis coefficients of the Lagrangian interpolation are the same as those of the L2 projection
        {
          ValueType diffErr = 0;
          auto hostBasisCoeffsL2DG = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace(), basisCoeffsL2DG);
          for(int k=0;k<basisCardinality;k++) {
            for(int ic=0; ic<numCells; ic++)
              diffErr = std::max(diffErr, std::abs(hostBasisCoeffsLI(ic,k) - hostBasisCoeffsL2DG(ic,k)));
          }

          if(diffErr > pow(16, degree)*tol) { //heuristic relation on how round-off error depends on degree
            errorFlag++;
            *outStream << std::setw(70) << "^^^^----FAILURE!" << "\n";
            *outStream << "HVOL_C" << degree << ": The weights recovered with the L2DG optimization are different than the one used for generating the functon."<<
                "\nThe max The infinite norm of the difference between the weights is: " <<  diffErr << std::endl;
          }
        }
        delete basisPtr;
      }
    }
  } catch (std::exception &err) {
    std::cout << " Exeption\n";
    *outStream << err.what() << "\n\n";
    errorFlag = -1000;
  }


  if (errorFlag != 0)
    std::cout << "End Result: TEST FAILED = " << errorFlag << "\n";
  else
    std::cout << "End Result: TEST PASSED\n";

  // reset format state of std::cout
  std::cout.copyfmt(oldFormatState);

  return errorFlag;
}
}
}

