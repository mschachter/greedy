/*=========================================================================

  Program:   Insight Segmentation & Registration Toolkit
  Module:    $RCSfile: SimpleWarpImageFilter.h,v $
  Language:  C++
  Date:      $Date: 2009-10-29 11:19:00 $
  Version:   $Revision: 1.31 $

  Copyright (c) Insight Software Consortium. All rights reserved.
  See ITKCopyright.txt or http://www.itk.org/HTML/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even 
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR 
     PURPOSE.  See the above copyright notices for more information.

=========================================================================*/
#ifndef __MultiImageRegistrationHelper_h
#define __MultiImageRegistrationHelper_h

#include "itkImageBase.h"
#include "itkVectorImage.h"
#include "itkMatrixOffsetTransformBase.h"



/**
 * This class is used to perform mean square intensity difference type
 * registration with multiple images. The filter is designed for speed
 * of interpolation.
 */
template <class TFloat, unsigned int VDim>
class MultiImageOpticalFlowHelper
{
public:

  typedef itk::VectorImage<TFloat, VDim> MultiComponentImageType;
  typedef itk::Image<TFloat, VDim> FloatImageType;
  typedef itk::CovariantVector<TFloat, VDim> VectorType;
  typedef itk::Image<VectorType, VDim> VectorImageType;
  typedef itk::ImageBase<VDim> ImageBaseType;

  typedef std::vector<int> PyramidFactorsType;
  typedef itk::Size<VDim> SizeType;

  typedef itk::MatrixOffsetTransformBase<double, VDim, VDim> LinearTransformType;

  /** Set default (power of two) pyramid factors */
  void SetDefaultPyramidFactors(int n_levels);

  /** Set the pyramid factors - for multi-resolution (e.g., 8,4,2) */
  void SetPyramidFactors(const PyramidFactorsType &factors);

  /** Add a pair of multi-component images to the class - same weight for each component */
  void AddImagePair(MultiComponentImageType *fixed, MultiComponentImageType *moving, double weight);

  /** Compute the composite image - must be run before any sampling is done */
  void BuildCompositeImages(bool add_noise);

  /** Get the reference image for level k */
  ImageBaseType *GetReferenceSpace(int level);

  /** Get the reference image for level k */
  ImageBaseType *GetMovingReferenceSpace(int level);

  /** Perform interpolation - compute [(I - J(Tx)) GradJ(Tx)] */
  double ComputeOpticalFlowField(int level, VectorImageType *def, VectorImageType *result,
                                 double result_scaling = 1.0);

  /** Compute normalized cross-correlation metric and gradient */
  double ComputeNCCMetricAndGradient(int level, VectorImageType *def, VectorImageType *result,
                                     const SizeType &radius, double result_scaling = 1.0);

  double ComputeAffineMatchAndGradient(int level, LinearTransformType *tran,
                                       LinearTransformType *grad = NULL);

  static void AffineToField(LinearTransformType *tran, VectorImageType *def);

  void VoxelWarpToPhysicalWarp(int level, VectorImageType *warp, VectorImageType *result);

protected:

  // Pyramid factors
  PyramidFactorsType m_PyramidFactors;

  // Weights
  std::vector<double> m_Weights;

  // Vector of images
  typedef std::vector<typename MultiComponentImageType::Pointer> MultiCompImageSet;

  // Fixed and moving images
  MultiCompImageSet m_Fixed, m_Moving;

  // Composite image at each resolution level
  MultiCompImageSet m_FixedComposite, m_MovingComposite;

  // Working memory image for NCC computation
  typename MultiComponentImageType::Pointer m_NCCWorkingImage;

  void PlaceIntoComposite(FloatImageType *src, MultiComponentImageType *target, int offset);
  void PlaceIntoComposite(VectorImageType *src, MultiComponentImageType *target, int offset);
};


#ifndef ITK_MANUAL_INSTANTIATION
#include "MultiImageRegistrationHelper.txx"
#endif

#endif
