/*=========================================================================

  Program:   ALFABIS fast medical image registration programs
  Language:  C++
  Website:   github.com/pyushkevich/greedy
  Copyright (c) Paul Yushkevich, University of Pennsylvania. All rights reserved.

  This program is part of ALFABIS: Adaptive Large-Scale Framework for
  Automatic Biomedical Image Segmentation.

  ALFABIS development is funded by the NIH grant R01 EB017255.

  ALFABIS is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  ALFABIS is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with ALFABIS.  If not, see <http://www.gnu.org/licenses/>.

=========================================================================*/
#include "CommandLineHelper.h"
#include "ShortestPath.h"

#include <iostream>
#include <sstream>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
#include <numeric>
#include <cerrno>

#include "itkMatrixOffsetTransformBase.h"
#include "itkImageAlgorithm.h"
#include "itkZeroFluxNeumannPadImageFilter.h"
#include "itkImageFileReader.h"

#include "lddmm_common.h"
#include "lddmm_data.h"

struct StackParameters
{
  bool reuse;
  std::string output_dir;
  StackParameters()
    : reuse(false) {}
};

struct SliceData
{
  std::string raw_filename;
  std::string unique_id;
  double z_pos;
};



/**
 * This class represents a reference to an image that may exist on disk, or may be
 * stored in memory. There is a limit on the amount of memory that can be used by
 * all the image refs, and images are rotated in and out of memory based on when
 * they were last accessed
 */
class ImageCache
{
public:

  ImageCache(unsigned long max_memory = 0l, unsigned int max_images = 0)
    : m_MaxMemory(max_memory), m_MaxImages(max_images), m_Counter(0l) {}

  template <typename TImage> typename TImage::Pointer GetImage(const std::string &filename)
  {
    // Check the cache for the image
    auto it = m_Cache.find(filename);
    if(it != m_Cache.end())
      {
      TImage *image = dynamic_cast<TImage *>(std::get<2>(it->second).GetPointer());
      if(!image)
        throw GreedyException("Type mismatch in image cache");
      typename TImage::Pointer image_ptr = image;
      return image_ptr;
      }

    // Image does not exist in cache, load it
    typedef itk::ImageFileReader<TImage> ReaderType;
    typename ReaderType::Pointer reader = ReaderType::New();
    reader->SetFileName(filename.c_str());
    reader->Update();
    typename TImage::Pointer image_ptr = reader->GetOutput();

    // Get the size of the image in bytes
    unsigned long img_size = image_ptr->GetPixelContainer()->Size()
                             * sizeof (typename TImage::PixelContainer::Element);

    // If the size of the image is too large, we need to reduce the size of the cache
    this->ShrinkCache(img_size, 1);

    // Add the new image
    m_Cache[filename] = std::make_tuple(m_Counter++, img_size, image_ptr);
    m_UsedMemory += img_size;

    // Return the image
    return image_ptr;
  }

  void ShrinkCache(unsigned long new_bytes, unsigned int new_images)
  {
    // Remove something from the cache until it's not empty and the constraints of the
    // cache are satisfied
    while(IsCacheFull(new_bytes, new_images) && m_Cache.size() > 0)
      {
      // Find the oldest entry in the cache
      std::map<unsigned long, std::string> sort_map;
      for(auto it : m_Cache)
        sort_map[std::get<0>(it.second)] = it.first;

      // Remove the first (oldest) entry
      auto it_erase = m_Cache.find(sort_map.begin()->second);
      m_UsedMemory -= std::get<1>(it_erase->second);
      m_Cache.erase(it_erase);
      }
  }

  bool IsCacheFull(unsigned long new_bytes, unsigned int new_images)
  {
    if(m_MaxMemory > 0 && m_UsedMemory + new_bytes > m_MaxMemory)
      return true;

    if(m_MaxImages > 0 && m_Cache.size() + new_images > m_MaxImages)
      return true;

    return false;
  }

  void PurgeCache()
  {
    m_Cache.clear();
    m_UsedMemory = 0;
  }

protected:

  // Cache entry (age, size, pointer)
  typedef std::tuple<unsigned long, unsigned long, itk::Object::Pointer> CacheEntry;
  typedef std::map<std::string, CacheEntry> CacheType;

  // Cache for images
  CacheType m_Cache;

  unsigned long m_MaxMemory, m_UsedMemory;
  unsigned int m_MaxImages;
  unsigned long m_Counter;
};


// How to specify how many neighbors a slice will be registered to?
// - minimum of one neighbor
// - maximum of <user_specified> neighbors
// - maximum offset

int usage(const std::string &stage = std::string())
{
  std::map<std::string, std::string> utext;
  utext[std::string()] = {
    #include "stackg_usage_main.h"
    0x00
  };

  utext["init"] = {
    #include "stackg_usage_init.h"
    0x00
  };

  utext["recon"] = {
    #include "stackg_usage_recon.h"
    0x00
  };

  utext["volmatch"] =  {
    #include "stackg_usage_volmatch.h"
    0x00
  };

  utext["voliter"] =  {
    #include "stackg_usage_voliter.h"
    0x00
  };

  utext["splat"] =  {
    #include "stackg_usage_splat.h"
    0x00
  };

  if(utext.find(stage) == utext.end())
    std::cout << utext[std::string()] << std::endl;
  else
    std::cout << utext[stage] << std::endl;
  return -1;
}


/**
 * A representation of the project
 */
class StackGreedyProject
{
public:

  // API typedefs
  typedef LDDMMData<double, 2> LDDMMType;
  typedef LDDMMData<double, 3> LDDMMType3D;
  typedef GreedyApproach<2, double> GreedyAPI;

  // Image typedefs
  typedef LDDMMType::CompositeImageType SlideImageType;
  typedef LDDMMType::CompositeImagePointer SlideImagePointer;
  typedef LDDMMType3D::CompositeImageType VolumeImage;
  typedef LDDMMType3D::CompositeImagePointer VolumePointer;


  /** Set of enums used to refer to files in the project directory */
  enum FileIntent {
    MANIFEST_FILE = 0, CONFIG_ENTRY, AFFINE_MATRIX, METRIC_VALUE, ACCUM_MATRIX, ACCUM_RESLICE,
    VOL_INIT_MATRIX, VOL_SLIDE, VOL_MEDIAN_INIT_MATRIX,
    VOL_ITER_MATRIX, VOL_ITER_WARP, ITER_METRIC_DUMP
  };

  /** Constructor */
  StackGreedyProject(std::string project_dir, const StackParameters &param)
  {
    m_ProjectDir = project_dir;
    m_GlobalParam = param;
  }

  /** Initialize the project */
  void InitializeProject(std::string fn_manifest, std::string default_ext = "nii.gz")
  {
    // Read the manifest and write a copy to the project dir
    this->ReadManifest(fn_manifest);
    this->WriteManifest(GetFilenameForGlobal(MANIFEST_FILE));

    // Read the default extension and save it
    this->m_DefaultImageExt = default_ext;
    this->SaveConfigKey("DefaultImageExt", m_DefaultImageExt);

    // Report what has been done
    printf("stack_greedy: Project initialized in %s\n", m_ProjectDir.c_str());
  }

  /** Restore the initialized project */
  void RestoreProject()
  {
    this->ReadManifest(GetFilenameForGlobal(MANIFEST_FILE));
    m_DefaultImageExt = this->LoadConfigKey("DefaultImageExt", std::string(".nii.gz"));
  }

  std::string GetFilenameForSlicePair(
      const SliceData &ref, const SliceData &mov, FileIntent intent)
  {
    char filename[1024];
    const char *dir = m_ProjectDir.c_str(), *ext = m_DefaultImageExt.c_str();
    const char *rid = ref.unique_id.c_str(), *mid = mov.unique_id.c_str();

    switch(intent)
      {
      case AFFINE_MATRIX:
        sprintf(filename, "%s/recon/nbr/affine_ref_%s_mov_%s.mat", dir, rid, mid);
        break;
      case METRIC_VALUE:
        sprintf(filename, "%s/recon/nbr/affine_ref_%s_mov_%s_metric.txt", dir, rid, mid);
        break;
      default:
        throw GreedyException("Wrong intent in GetFilenameForSlicePair");
      }

    // Make sure the directory containing this exists
    itksys::SystemTools::MakeDirectory(itksys::SystemTools::GetFilenamePath(filename));

    return filename;
  }

  std::string GetFilenameForSlice(const SliceData &slice, int intent, ...)
  {
    char filename[1024];
    const char *dir = m_ProjectDir.c_str(), *ext = m_DefaultImageExt.c_str();
    const char *sid = slice.unique_id.c_str();

    va_list args;
    va_start(args, intent);

    int iter =
        (intent == VOL_ITER_MATRIX || intent == VOL_ITER_WARP || intent == ITER_METRIC_DUMP)
        ? va_arg(args, int) : 0;

    switch(intent)
      {
      case ACCUM_MATRIX:
        sprintf(filename, "%s/recon/accum/accum_affine_%s.mat", dir, sid);
        break;
      case ACCUM_RESLICE:
        sprintf(filename, "%s/recon/accum/accum_affine_%s_reslice.%s", dir, sid, ext);
        break;
      case VOL_INIT_MATRIX:
        sprintf(filename, "%s/vol/match/affine_refvol_mov_%s.mat", dir, sid);
        break;
      case VOL_SLIDE:
        sprintf(filename, "%s/vol/slides/vol_slide_%s.%s", dir, sid, ext);
        break;
      case VOL_ITER_MATRIX:
        sprintf(filename, "%s/vol/iter%02d/affine_refvol_mov_%s_iter%02d.mat", dir, iter, sid, iter);
        break;
      case VOL_ITER_WARP:
        sprintf(filename, "%s/vol/iter%02d/warp_refvol_mov_%s_iter%02d.%s", dir, iter, sid, iter, ext);
        break;
      case ITER_METRIC_DUMP:
        sprintf(filename, "%s/vol/iter%02d/metric_refvol_mov_%s_iter%02d.txt", dir, iter, sid, iter);
        break;
      default:
        throw GreedyException("Wrong intent in GetFilenameForSlice");
      }

    va_end(args);

    // Make sure the directory containing this exists
    itksys::SystemTools::MakeDirectory(itksys::SystemTools::GetFilenamePath(filename));

    return filename;
  }

  std::string GetFilenameForGlobal(int intent, ...)
  {
    char filename[1024];
    const char *dir = m_ProjectDir.c_str(), *ext = m_DefaultImageExt.c_str();

    va_list args;
    va_start(args, intent);

    switch(intent)
      {
      case VOL_MEDIAN_INIT_MATRIX:
        sprintf(filename, "%s/vol/match/affine_refvol_median.mat", dir);
        break;
      case MANIFEST_FILE:
        sprintf(filename, "%s/config/manifest.txt", dir);
        break;
      case CONFIG_ENTRY:
        sprintf(filename, "%s/config/dict/%s", dir, va_arg(args, char *));
        break;
      default:
        throw GreedyException("Wrong intent in GetFilenameForGlobal");
      }

    va_end(args);

    // Make sure the directory containing this exists
    itksys::SystemTools::MakeDirectory(itksys::SystemTools::GetFilenamePath(filename));

    return filename;
  }

  template <typename T> void SaveConfigKey(const std::string &key, const T &value)
  {
    std::string fn = GetFilenameForGlobal(CONFIG_ENTRY, key.c_str());
    std::ofstream fout(fn);
    fout << value;
  }

  template <typename T> T LoadConfigKey(const std::string &key, const T &def_value)
  {
    std::string fn = GetFilenameForGlobal(CONFIG_ENTRY, key.c_str());
    std::ifstream fin(fn);
    T value;
    if(fin.good())
      fin >> value;
    else
      value = def_value;
    return value;
  }

  void ReadManifest(const std::string &fn_manifest)
  {
    // Reset the slices
    m_Slices.clear();
    m_SortedSlices.clear();

    // Read the manifest file
    std::ifstream fin(fn_manifest);
    std::string f_line;
    while(std::getline(fin, f_line))
      {
      // Read the values from the manifest
      std::istringstream iss(f_line);
      SliceData slice;
      if(!(iss >> slice.unique_id >> slice.z_pos >> slice.raw_filename))
        throw GreedyException("Error reading manifest file, line %s", f_line.c_str());

      // Check that the manifest points to a real file
      if(!itksys::SystemTools::FileExists(slice.raw_filename.c_str(), true))
        throw GreedyException("File %s referenced in the manifest does not exist", slice.raw_filename.c_str());

      // Get an absolute filename
      slice.raw_filename = itksys::SystemTools::CollapseFullPath(slice.raw_filename.c_str());

      // Add to sorted list
      m_SortedSlices.insert(std::make_pair(slice.z_pos, m_Slices.size()));
      m_Slices.push_back(slice);
      }
  }

  bool CanSkipFile(const std::string &fn)
  {
    return m_GlobalParam.reuse && itksys::SystemTools::FileExists(fn.c_str(), true);
  }

  void WriteManifest(const std::string &fn_manifest)
  {
    std::ofstream fout(fn_manifest);
    for(auto slice : m_Slices)
      fout << slice.unique_id << " " << slice.z_pos << " " << slice.raw_filename << std::endl;
  }

  void ReconstructStack(double z_range, double z_epsilon, const GreedyParameters &gparam)
  {
    // Configure the threads
    GreedyAPI::ConfigThreads(gparam);

    // Store the z-parameters (although we probably do not need them)
    this->SaveConfigKey("Z_Range", z_range);
    this->SaveConfigKey("Z_Epsilon", z_epsilon);

    // We keep for each slice the list of z-sorted neigbors
    std::vector<slice_ref_set> slice_nbr(m_Slices.size());
    unsigned int n_edges = 0;

    // Forward pass
    for(auto it = m_SortedSlices.begin(); it != m_SortedSlices.end(); ++it)
      {
      // Add at least the following slice
      auto it_next = it; ++it_next;
      unsigned int n_added = 0;

      // Now add all the slices in the range
      while(it_next != m_SortedSlices.end()
            && (n_added < 1 || fabs(it->first - it_next->first) < z_range))
        {
        slice_nbr[it->second].insert(*it_next);
        n_added++;
        ++it_next;
        n_edges++;
        }
      }

    // Forward pass
    for(auto it = m_SortedSlices.rbegin(); it != m_SortedSlices.rend(); ++it)
      {
      // Add at least the following slice
      auto it_next = it; ++it_next;
      unsigned int n_added = 0;

      // Now add all the slices in the range
      while(it_next != m_SortedSlices.rend()
            && (n_added < 1 || fabs(it->first - it_next->first) < z_range))
        {
        slice_nbr[it->second].insert(*it_next);
        n_added++;
        ++it_next;
        n_edges++;
        }
      }

    // Set up a cache for loaded images. These images can be cycled in and out of memory
    // depending on need. TODO: let user configure cache sizes
    ImageCache slice_cache(0, 20);

    // At this point we can create a rigid adjacency structure for the graph-theoretic algorithm,
    vnl_vector<unsigned int> G_adjidx(m_SortedSlices.size()+1, 0u);
    vnl_vector<unsigned int> G_adj(n_edges, 0u);
    vnl_vector<double> G_edge_weight(n_edges, DijkstraShortestPath<double>::INFINITE_WEIGHT);

    for(unsigned int k = 0, p = 0; k < m_Slices.size(); k++)
      {
      G_adjidx[k+1] = G_adjidx[k] + (unsigned int) slice_nbr[k].size();
      for(auto it : slice_nbr[k])
        G_adj[p++] = it.second;
      }

    // Set up the graph of all registrations. Each slice is a node and edges are between each
    // slice and its closest slices, as well as between each slice and slices in the z-range
    typedef std::tuple<unsigned int, unsigned int, double> GraphEdge;
    std::set<GraphEdge> slice_graph;

    // Perform rigid registration between pairs of images. We should do this in a way that
    // the number of images loaded and unloaded is kept to a minimum, without filling memory.
    // The best way to do so would be to progress in z order and release images that are too
    // far behind in z to be included for the current 'reference' image
    for(auto it : m_SortedSlices)
      {
      const auto &nbr = slice_nbr[it.second];

      // Read the reference slide from the cache
      SlideImagePointer i_ref =
          slice_cache.GetImage<SlideImageType>(m_Slices[it.second].raw_filename);

      // Iterate over the neighbor slices
      unsigned int n_pos = 0;
      for(auto it_n : nbr)
        {
        // Load or retrieve the corresponding image
        SlideImagePointer i_mov =
            slice_cache.GetImage<SlideImageType>(m_Slices[it_n.second].raw_filename);

        // Get the filenames that will be generated by registration
        std::string fn_matrix = GetFilenameForSlicePair(m_Slices[it.second], m_Slices[it_n.second], AFFINE_MATRIX);
        std::string fn_metric = GetFilenameForSlicePair(m_Slices[it.second], m_Slices[it_n.second], METRIC_VALUE);
        double pair_metric = 1e100;

        // Perform registration or reuse existing registration results
        if(CanSkipFile(fn_matrix) && CanSkipFile(fn_metric))
          {
          std::ifstream fin(fn_metric);
          fin >> pair_metric;
          }
        else
          {
          // Perform the registration between i_ref and i_mov
          GreedyAPI greedy_api;

          // Make a copy of the template parameters
          GreedyParameters my_param = gparam;

          // Set up the image pair for registration
          ImagePairSpec img_pair(m_Slices[it.second].raw_filename, m_Slices[it_n.second].raw_filename);
          greedy_api.AddCachedInputObject(m_Slices[it.second].raw_filename, i_ref.GetPointer());
          greedy_api.AddCachedInputObject(m_Slices[it_n.second].raw_filename, i_mov.GetPointer());
          my_param.inputs.push_back(img_pair);

          // Set other parameters
          my_param.affine_dof = GreedyParameters::DOF_RIGID;
          my_param.affine_init_mode = IMG_CENTERS;

          // Set up the output of the affine
          my_param.output = fn_matrix;

          // Perform affine/rigid
          printf("#############################\n");
          printf("### Fixed :%s   Moving %s ###\n", m_Slices[it.second].unique_id.c_str(),m_Slices[it_n.second].unique_id.c_str());
          printf("#############################\n");
          greedy_api.RunAffine(my_param);

          // Get the metric for the affine registration
          pair_metric = greedy_api.GetLastMetricReport().TotalMetric;
          std::cout << "Last metric value: " << pair_metric << std::endl;

          // Normalize the metric to give the actual mean NCC
          pair_metric /= -10000.0 * i_ref->GetNumberOfComponentsPerPixel();
          std::ofstream f_metric(fn_metric);
          f_metric << pair_metric << std::endl;
          }

        // Map the metric value into a weight
        double weight = (1.0 - pair_metric) * pow(1 + z_epsilon, fabs(it_n.first - it.first));

        // Regardless of whether we did registration or not, record the edge in the graph
        G_edge_weight[G_adjidx[it.second] + n_pos++] = weight;
        }
      }

    // Run the shortest path computations
    DijkstraShortestPath<double> dijkstra((unsigned int) m_Slices.size(),
                                          G_adjidx.data_block(), G_adj.data_block(), G_edge_weight.data_block());

    // Compute the shortest paths from every slice to the rest and record the total distance. This will
    // help generate the root of the tree
    unsigned int i_root = 0;
    double best_root_dist = 0.0;
    for(unsigned int i = 0; i < m_Slices.size(); i++)
      {
      dijkstra.ComputePathsFromSource(i);
      double root_dist = 0.0;
      for(unsigned int j = 0; j < m_Slices.size(); j++)
        root_dist += dijkstra.GetDistanceArray()[j];
      std::cout << "Root distance " << i << " : " << root_dist << std::endl;
      if(i == 0 || best_root_dist > root_dist)
        {
        i_root = i;
        best_root_dist = root_dist;
        }
      }

    // Compute the composed transformations between the root and each of the inputs
    dijkstra.ComputePathsFromSource(i_root);

    // Load the root image into memory
    LDDMMType::ImagePointer img_root;
    LDDMMType::img_read(m_Slices[i_root].raw_filename.c_str(), img_root);

    // Apply some padding to the root image.
    typedef itk::ZeroFluxNeumannPadImageFilter<LDDMMType::ImageType, LDDMMType::ImageType> PadFilter;
    PadFilter::Pointer fltPad = PadFilter::New();
    fltPad->SetInput(img_root);

    // Determine the amount of padding to add
    unsigned int max_dim =
        std::max(img_root->GetBufferedRegion().GetSize()[0],img_root->GetBufferedRegion().GetSize()[1]);
    itk::Size<2> pad_size;
    pad_size.Fill(max_dim / 4);
    fltPad->SetPadBound(pad_size);
    fltPad->Update();

    // Store the result
    LDDMMType::ImagePointer img_root_padded = fltPad->GetOutput();

    // The padded image has a non-zero index, which causes problems downstream for GreedyAPI.
    // To account for this, we save and load the image
    // TODO: handle this internally using a filter!
    LDDMMType::img_write(img_root_padded, "/tmp/padded.nii.gz");
    img_root_padded = LDDMMType::img_read("/tmp/padded.nii.gz");

    // Compute transformation for each slice
    for(unsigned int i = 0; i < m_Slices.size(); i++)
      {
      // Initialize the total transform matrix
      vnl_matrix<double> t_accum(3, 3, 0.0);
      t_accum.set_identity();

      // Traverse the path
      unsigned int i_curr = i, i_prev = dijkstra.GetPredecessorArray()[i];
      std::cout << "Chain for " << i << " : ";
      while(i_prev != DijkstraShortestPath<double>::NO_PATH && (i_prev != i_curr))
        {
        // Load the matrix
        std::string fn_matrix =
            GetFilenameForSlicePair(m_Slices[i_prev], m_Slices[i_curr], AFFINE_MATRIX);
        vnl_matrix<double> t_step = GreedyAPI::ReadAffineMatrix(TransformSpec(fn_matrix));

        // Accumulate the total transformation
        t_accum = t_accum * t_step;

        std::cout << i_prev << " ";

        // Go to the next edge
        i_curr = i_prev;
        i_prev = dijkstra.GetPredecessorArray()[i_curr];
        }

      std::cout << std::endl;

      // Store the accumulated transform
      std::string fn_accum_matrix = GetFilenameForSlice(m_Slices[i], ACCUM_MATRIX);
      GreedyAPI::WriteAffineMatrix(fn_accum_matrix, t_accum);

      // Write a resliced image
      std::string fn_accum_reslice = GetFilenameForSlice(m_Slices[i], ACCUM_RESLICE);

      // Hold the resliced image in memory
      LDDMMType::CompositeImagePointer img_reslice = LDDMMType::CompositeImageType::New();

      // Only do reslice if necessary
      if(!CanSkipFile(fn_accum_reslice))
        {
        // Perform the registration between i_ref and i_mov
        GreedyAPI greedy_api;

        // Make a copy of the template parameters
        GreedyParameters my_param = gparam;

        // Set up the image pair for registration
        my_param.reslice_param.ref_image = "root_slice_padded";
        my_param.reslice_param.images.push_back(ResliceSpec(m_Slices[i].raw_filename, fn_accum_reslice));
        my_param.reslice_param.transforms.push_back(TransformSpec(fn_accum_matrix));
        greedy_api.AddCachedInputObject("root_slice_padded", img_root_padded.GetPointer());
        greedy_api.AddCachedInputObject(m_Slices[i].raw_filename,
                                        slice_cache.GetImage<SlideImageType>(m_Slices[i].raw_filename));
        greedy_api.AddCachedOutputObject(fn_accum_reslice, img_reslice.GetPointer(), true);
        greedy_api.RunReslice(my_param);
        }
      else
        {
        // Just read the resliced image
        img_reslice = LDDMMType::cimg_read(fn_accum_reslice.c_str());
        }
      }
  }

  static SlideImagePointer ExtractSliceFromVolume(VolumePointer vol, double z_pos)
  {
    VolumePointer vol_slice = LDDMMType3D::CompositeImageType::New();
    typename LDDMMType3D::RegionType reg_slice = vol->GetBufferedRegion();
    reg_slice.GetModifiableSize()[2] = 1;
    vol_slice->CopyInformation(vol);
    vol_slice->SetRegions(reg_slice);
    vol_slice->Allocate();

    // Adjust the origin of the slice
    auto origin_slice = vol_slice->GetOrigin();
    origin_slice[2] = z_pos;
    vol_slice->SetOrigin(origin_slice);

    // Generate a blank deformation field
    LDDMMType3D::VectorImagePointer zero_warp = LDDMMType3D::new_vimg(vol_slice);

    // Sample the slice from the volume
    LDDMMType3D::interp_cimg(vol, zero_warp, vol_slice, false, true, 0.0);

    // Now drop the dimension of the slice to 2D
    LDDMMType::RegionType reg_slice_2d;
    LDDMMType::CompositeImageType::PointType origin_2d;
    LDDMMType::CompositeImageType::SpacingType spacing_2d;
    LDDMMType::CompositeImageType::DirectionType dir_2d;

    for(unsigned int a = 0; a < 2; a++)
      {
      reg_slice_2d.SetIndex(a, reg_slice.GetIndex(a));
      reg_slice_2d.SetSize(a, reg_slice.GetSize(a));
      origin_2d[a] = vol_slice->GetOrigin()[a];
      spacing_2d[a] = vol_slice->GetSpacing()[a];
      dir_2d(a,0) = vol_slice->GetDirection()(a,0);
      dir_2d(a,1) = vol_slice->GetDirection()(a,1);
      }

    SlideImagePointer vol_slice_2d = SlideImageType::New();
    vol_slice_2d->SetRegions(reg_slice_2d);
    vol_slice_2d->SetOrigin(origin_2d);
    vol_slice_2d->SetDirection(dir_2d);
    vol_slice_2d->SetSpacing(spacing_2d);
    vol_slice_2d->SetNumberOfComponentsPerPixel(vol_slice->GetNumberOfComponentsPerPixel());
    vol_slice_2d->Allocate();

    // Copy data between the pixel containers
    itk::ImageAlgorithm::Copy(vol_slice.GetPointer(), vol_slice_2d.GetPointer(),
                              vol_slice->GetBufferedRegion(), vol_slice_2d->GetBufferedRegion());

    return vol_slice_2d;
  }

  void InitialMatchToVolume(const std::string &fn_volume, const GreedyParameters &gparam)
  {
    // Configure the threads
    GreedyAPI::ConfigThreads(gparam);

    // Read the 3D volume into memory
    LDDMMType3D::CompositeImagePointer vol;
    LDDMMType3D::cimg_read(fn_volume.c_str(), vol);

    // Extract target slices from the 3D volume
    for(unsigned int i = 0; i < m_Slices.size(); i++)
      {
      // Filename for the volume slice corresponding to current slide
      std::string fn_vol_slide = GetFilenameForSlice(m_Slices[i], VOL_SLIDE);

      // Output matrix for this registration
      std::string fn_vol_init_matrix = GetFilenameForSlice(m_Slices[i], VOL_INIT_MATRIX);

      if(!CanSkipFile(fn_vol_slide) || !CanSkipFile(fn_vol_init_matrix))
        {
        // Extract the slice from the 3D image
        SlideImagePointer vol_slice_2d = ExtractSliceFromVolume(vol, m_Slices[i].z_pos);

        // Write the 2d slice
        LDDMMType::cimg_write(vol_slice_2d, fn_vol_slide.c_str());

        // Try registration between resliced slide and corresponding volume slice with
        // a brute force search. This will be used to create a median transformation
        // between slide space and volume space. Since the volume may come with a mask,
        // we use volume slice as fixed, and the slide image as moving
        GreedyAPI greedy_api;
        GreedyParameters my_param = gparam;

        // Set up the image pair for registration
        std::string fn_accum_reslice = GetFilenameForSlice(m_Slices[i], ACCUM_RESLICE);

        ImagePairSpec img_pair("vol_slice", fn_accum_reslice, 1.0);
        greedy_api.AddCachedInputObject("vol_slice", vol_slice_2d.GetPointer());
        my_param.inputs.push_back(img_pair);

        // Set other parameters
        my_param.affine_dof = GreedyParameters::DOF_AFFINE;
        my_param.affine_init_mode = IMG_CENTERS;

        // Set up the output of the affine
        my_param.output = fn_vol_init_matrix;

        // Run the affine registration
        greedy_api.RunAffine(my_param);
        }
      }

    // List of affine matrices to the volume slice
    typedef vnl_matrix_fixed<double, 3, 3> Mat3;
    std::vector<Mat3> vol_affine(m_Slices.size());
    for(unsigned int i = 0; i < m_Slices.size(); i++)
      {
      std::string fn_vol_init_matrix = GetFilenameForSlice(m_Slices[i], VOL_INIT_MATRIX);
      vol_affine[i] = GreedyAPI::ReadAffineMatrix(TransformSpec(fn_vol_init_matrix));
      }

    // Compute distances between all pairs of affine matrices
    vnl_matrix<double> aff_dist(m_Slices.size(), m_Slices.size()); aff_dist.fill(0.0);
    for(unsigned int i = 0; i < m_Slices.size(); i++)
      {
      for(unsigned int j = 0; j < i; j++)
        {
        aff_dist(i,j) = (vol_affine[i] - vol_affine[j]).array_one_norm();
        aff_dist(j,i) = aff_dist(i,j);
        }
      }

    // Compute the sum of distances from each matrix to the rest
    vnl_vector<double> row_sums = aff_dist * vnl_vector<double>(m_Slices.size(), 1.0);

    // Find the index of the smallest element
    unsigned int idx_best =
        std::find(row_sums.begin(), row_sums.end(), row_sums.min_value()) -
        row_sums.begin();

    // The median affine
    Mat3 median_affine = vol_affine[idx_best];

    // Write the median affine to a file
    GreedyAPI::WriteAffineMatrix(GetFilenameForGlobal(VOL_MEDIAN_INIT_MATRIX), median_affine);

    // Now write the complete initial to-volume transform for each slide
    for(unsigned int i = 0; i < m_Slices.size(); i++)
      {
      vnl_matrix<double> M_root =
          GreedyAPI::ReadAffineMatrix(GetFilenameForSlice(m_Slices[i], ACCUM_MATRIX));

      vnl_matrix<double> M_vol = M_root * median_affine;

      GreedyAPI::WriteAffineMatrix(
            GetFilenameForSlice(m_Slices[i], VOL_ITER_MATRIX, 0), M_vol);
      }
  }

  // Now that we have the affine initialization from the histology space to the volume space, we can
  // perform iterative optimization, where each slice is matched to its neighbors and to the
  // corresponding MRI slice. The only issue here is how do we want to use the graph in this
  // process: we don't want the bad neighbors to pull the registration away from the good
  // solution. On the other hand, we can expect the bad slices to eventually auto-correct. It seems
  // that the proper approach would be to down-weigh certain slices by their metric, but then
  // again, do we want to do this based on initial metric or current metric. For now, we can start
  // by just using same weights.
  void IterativeMatchToVolume(unsigned int n_affine, unsigned int n_deform, unsigned int i_first,
                              unsigned int i_last, double w_volume, const GreedyParameters &gparam)
  {
    // Configure the threads
    GreedyAPI::ConfigThreads(gparam);

    // Set up a cache for loaded images. These images can be cycled in and out of memory
    // depending on need. TODO: let user configure cache sizes
    ImageCache slice_cache(0, 20);

    // What iteration?
    if(i_first > i_last || i_first == 0 || i_last > n_affine + n_deform)
      throw GreedyException("Iteration range (%d, %d) is out of range [1, %d]",
                            i_first, i_last, n_affine + n_deform);

    // Iterate
    for(unsigned int iter = i_first; iter <= i_last; ++iter)
      {
      // Randomly shuffle the order in which slices are considered
      std::vector<unsigned int> ordering(m_Slices.size());
      std::iota(ordering.begin(), ordering.end(), 0);
      std::random_shuffle(ordering.begin(), ordering.end());

      // Keep track of the total neighbor metric and total volume metric
      double total_to_nbr_metric = 0.0;
      double total_to_vol_metric = 0.0;

      // Iterate over the ordering
      for(unsigned int k : ordering)
        {
        // The output filename for this affine registration
        std::string fn_result =
            iter <= n_affine
            ? GetFilenameForSlice(m_Slices[k], VOL_ITER_MATRIX, iter)
            : GetFilenameForSlice(m_Slices[k], VOL_ITER_WARP, iter);

        // Has this already been done? Then on to the next!
        if(CanSkipFile(fn_result))
          continue;

        // Get the pointer to the current slide (used as moving image)
        SlideImagePointer img_slide = slice_cache.GetImage<SlideImageType>(m_Slices[k].raw_filename);

        // Get the corresponding slice from the 3D volume (it's already saved in the project)
        SlideImagePointer vol_slice_2d = slice_cache.GetImage<SlideImageType>(
                                           GetFilenameForSlice(m_Slices[k], VOL_SLIDE));

        // Set up the registration. We are registering to the volume and to the transformed
        // adjacent slices. We should do everything in the space of the MRI volume because
        // (a) it should be large enough to cover the histology and (b) there might be a mask
        // in this space, while we cannot expect there to be a mask in the other space.

        // Find the adjacent slices. TODO: there is all kinds of stuff that could be done here,
        // like allowing a z-range for adjacent slices registration, modulating weight by the
        // distance, and detecting and down-weighting 'bad' slices. For now just pick the slices
        // immediately below and above the current slice
        auto itf = m_SortedSlices.find(std::make_pair(m_Slices[k].z_pos, k));
        if(itf == m_SortedSlices.end())
          throw GreedyException("Slice not found in sorted list (%d, z = %f)", k, m_Slices[k].z_pos);

        // Go backward and forward one slice
        slice_ref_set k_nbr;
        auto itf_back = itf, itf_fore = itf;
        if(itf != m_SortedSlices.begin())
          k_nbr.insert(*(--itf_back));
        if((++itf_fore) != m_SortedSlices.end())
          k_nbr.insert(*itf_fore);

        // Create the greedy API for the main registration task
        GreedyAPI api_reg;
        api_reg.AddCachedInputObject("moving", img_slide);
        api_reg.AddCachedInputObject("volume_slice", vol_slice_2d);

        // We need to hold on to the resliced image pointers, because otherwise they will be deallocated
        std::vector<SlideImagePointer> resliced_neighbors(m_Slices.size());

        // Set up the main registration pair
        GreedyParameters param_reg = gparam;
        param_reg.inputs.push_back(ImagePairSpec("volume_slice", "moving", w_volume));

        // Handle each of the neighbors
        for(auto nbr : k_nbr)
          {
          unsigned int j = nbr.second;

          // Create an image pointer for the reslicing output
          resliced_neighbors[j] = SlideImageType::New();

          // Each of the neighbor slices needs to be resliced using last iteration's transform. We
          // could cache these images, but then again, it does not take so much to do this on the
          // fly. For now we will do this on the fly.
          GreedyAPI api_reslice;
          api_reslice.AddCachedInputObject("vol_slice", vol_slice_2d);
          api_reslice.AddCachedOutputObject("output", resliced_neighbors[j], false);

          GreedyParameters param_reslice = gparam;
          param_reslice.reslice_param.ref_image = "vol_slice";
          param_reslice.reslice_param.images.push_back(ResliceSpec(m_Slices[j].raw_filename, "output"));

          // Was the previous iteration a deformable iteration? If so, apply the warp
          if(iter - 1 <= n_affine)
            {
            param_reslice.reslice_param.transforms.push_back(
                  TransformSpec(GetFilenameForSlice(m_Slices[j], VOL_ITER_MATRIX, iter-1)));
            }
          else
            {
            param_reslice.reslice_param.transforms.push_back(
                  TransformSpec(GetFilenameForSlice(m_Slices[j], VOL_ITER_WARP, iter-1)));
            param_reslice.reslice_param.transforms.push_back(
                  TransformSpec(GetFilenameForSlice(m_Slices[j], VOL_ITER_MATRIX, n_affine)));
            }

          // Perform the reslicing
          api_reslice.RunReslice(param_reslice);

          // Add the image pair to the registration
          char fixed_fn[64];
          sprintf(fixed_fn, "neighbor_%03d", j);
          api_reg.AddCachedInputObject(fixed_fn, resliced_neighbors[j]);

          param_reg.inputs.push_back(ImagePairSpec(fixed_fn, "moving", 1.0));
          }

        printf("#############################\n");
        printf("### Iter :%d   Slide %s ###\n", iter, m_Slices[k].unique_id.c_str());
        printf("#############################\n");

        // What kind of registration are we doing at this iteration?
        if(iter <= n_affine)
          {
          // Specify the DOF, etc
          param_reg.affine_dof = GreedyParameters::DOF_AFFINE;
          param_reg.affine_init_mode = RAS_FILENAME;
          param_reg.affine_init_transform =
              TransformSpec(GetFilenameForSlice(m_Slices[k], VOL_ITER_MATRIX, iter-1));
          param_reg.rigid_search = RigidSearchSpec();

          // Specify the output
          std::string fn_result = GetFilenameForSlice(m_Slices[k], VOL_ITER_MATRIX, iter);
          param_reg.output = fn_result;

          // Run this registration!
          api_reg.RunAffine(param_reg);
          }
        else
          {
          // Apply the last affine transformation
          param_reg.moving_pre_transforms.push_back(
                TransformSpec(GetFilenameForSlice(m_Slices[k], VOL_ITER_MATRIX, n_affine)));

          // Specify the output
          param_reg.output = fn_result;
          param_reg.affine_init_mode = VOX_IDENTITY;

          // Run the registration
          api_reg.RunDeformable(param_reg);
          }

        MultiComponentMetricReport last_metric_report = api_reg.GetLastMetricReport();
        total_to_vol_metric += last_metric_report.ComponentMetrics[0];
        for(unsigned int a = 1; a < last_metric_report.ComponentMetrics.size(); a++)
          total_to_nbr_metric += last_metric_report.ComponentMetrics[a];

        // Write the metric for this slide to file
        std::string fn_metric = GetFilenameForSlice(m_Slices[k], ITER_METRIC_DUMP, iter);
        std::ofstream fout(fn_metric);
        fout << api_reg.PrintIter(-1, -1, last_metric_report) << std::endl;
        }

      printf("ITER %3d  TOTAL_VOL_METRIC = %8.4f  TOTAL_NBR_METRIC = %8.4f\n",
             iter, total_to_vol_metric, total_to_nbr_metric);
      }
  }

private:

  // Path to the project
  std::string m_ProjectDir;

  // Default image file extension
  std::string m_DefaultImageExt;

  // Global parameters (parameters for the current run)
  StackParameters m_GlobalParam;

  // A flat list of slices (in manifest order)
  std::vector<SliceData> m_Slices;

  // A list of slices sorted by the z-position
  typedef std::pair<double, unsigned int> slice_ref;
  typedef std::set<slice_ref> slice_ref_set;
  slice_ref_set m_SortedSlices;




};


/**
 * Initialize the project
 */
void init(StackParameters &param, CommandLineHelper &cl)
{
  // Parse the parameters
  std::string arg;
  std::string fn_manifest;
  std::string default_ext = "nii.gz";
  while(cl.read_command(arg))
    {
    if(arg == "-M")
      {
      fn_manifest = cl.read_existing_filename();
      }
    else if(arg == "-ext")
      {
      default_ext = cl.read_string();
      }
    else
      throw GreedyException("Unknown parameter to 'init': %s", arg.c_str());
    }

  // Check required parameters
  if(fn_manifest.size() == 0)
    throw GreedyException("Missing manifest file (-M) in 'init'");

  // Create the project
  StackGreedyProject sgp(param.output_dir, param);
  sgp.InitializeProject(fn_manifest, default_ext);
}

/**
 * Run the reconstruction module
 */
void recon(StackParameters &param, CommandLineHelper &cl)
{
  // List of greedy commands that are recognized by this mode
  std::set<std::string> greedy_cmd {
    "-m", "-n", "-threads", "-gm-trim", "-search"
  };

  // Greedy parameters for this mode
  GreedyParameters gparam;

  // Parse the parameters
  double z_range = 0.0;
  double z_epsilon = 0.1;
  std::string arg;
  while(cl.read_command(arg))
    {
    if(arg == "-z")
      {
      z_range = cl.read_double();
      z_epsilon = cl.read_double();
      }
    else if(greedy_cmd.find(arg) != greedy_cmd.end())
      {
      gparam.ParseCommandLine(arg, cl);
      }
    else
      throw GreedyException("Unknown parameter to 'init': %s", arg.c_str());
    }

  // Create the project
  StackGreedyProject sgp(param.output_dir, param);
  sgp.RestoreProject();
  sgp.ReconstructStack(z_range, z_epsilon, gparam);
}


/**
 * Run the volume matching module
 */
void volmatch(StackParameters &param, CommandLineHelper &cl)
{
  // List of greedy commands that are recognized by this mode
  std::set<std::string> greedy_cmd {
    "-m", "-n", "-threads", "-gm-trim", "-search"
  };

  // Greedy parameters for this mode
  GreedyParameters gparam;

  // Parse the parameters
  std::string fn_volume;
  std::string arg;
  while(cl.read_command(arg))
    {
    if(arg == "-i")
      {
      fn_volume = cl.read_existing_filename();
      }
    else if(greedy_cmd.find(arg) != greedy_cmd.end())
      {
      gparam.ParseCommandLine(arg, cl);
      }
    else
      throw GreedyException("Unknown parameter to 'volmatch': %s", arg.c_str());
    }

  // Check required parameters
  if(fn_volume.size() == 0)
    throw GreedyException("Missing volume file (-i) in 'volmatch'");

  // Create the project
  StackGreedyProject sgp(param.output_dir, param);
  sgp.RestoreProject();
  sgp.InitialMatchToVolume(fn_volume, gparam);
}


/**
 * Run the iterative module
 */
void voliter(StackParameters &param, CommandLineHelper &cl)
{
  // List of greedy commands that are recognized by this mode
  std::set<std::string> greedy_cmd {
    "-m", "-n", "-threads", "-gm-trim", "-s", "-e", "-sv", "-exp"
  };

  // Greedy parameters for this mode
  GreedyParameters gparam;

  // Parse the parameters
  unsigned int n_affine = 5, n_deform = 5;
  unsigned int i_first = 0, i_last = 0;
  double w_volume = 4.0;

  std::string arg;
  while(cl.read_command(arg))
    {
    if(arg == "-R")
      {
      i_first = cl.read_integer();
      i_last = cl.read_integer();
      }
    else if(arg == "-na")
      {
      n_affine = cl.read_integer();
      }
    else if(arg == "-nd")
      {
      n_deform = cl.read_integer();
      }
    else if(arg == "-w")
      {
      w_volume = cl.read_double();
      }
    else if(greedy_cmd.find(arg) != greedy_cmd.end())
      {
      gparam.ParseCommandLine(arg, cl);
      }
    else
      throw GreedyException("Unknown parameter to 'voliter': %s", arg.c_str());
    }

  // Default is to run all iterations
  if(i_first == 0 && i_last == 0)
    {
    i_first = 1;
    i_last = n_affine + n_deform;
    }

  // Create the project
  StackGreedyProject sgp(param.output_dir, param);
  sgp.RestoreProject();
  sgp.IterativeMatchToVolume(n_affine, n_deform, i_first, i_last, w_volume, gparam);
}

int main(int argc, char *argv[])
{
  // Parameters specifically for this application
  StackParameters param;

  // Parameters for running Greedy in general
  GreedyParameters gparam;
  GreedyParameters::SetToDefaults(gparam);
  if(argc < 2)
    return usage();

  // Read the first command
  try
  {
  CommandLineHelper cl(argc, argv);

  // Read the global commands
  while(!cl.is_at_end() && cl.peek_arg()[0] == '-')
    {
    std::string arg = cl.read_command();
    if(arg == "-N")
      {
      param.reuse = true;
      }
    else
      {
      std::cerr << "Unknown global option " << arg << std::endl;
      return -1;
      }
    }

  // Read the main command
  if(cl.is_at_end())
    {
    std::cerr << "Missing command. Run this program without parameters to see usage." << std::endl;
    return -1;
    }

  std::string cmd = cl.read_string();

  // Is the string known
  if(cmd == "help")
    {
    return usage(cl.is_at_end() ? std::string() : cl.read_string());
    }

  // All commands other than 'help' end with the project directory. So we should get that
  // as the last argument from the command-line
  CommandLineHelper cl_end = cl.take_end(1);
  param.output_dir = cl_end.read_output_filename();

  if(cmd == "init")
    {
    init(param, cl);
    }
  else if(cmd == "recon")
    {
    recon(param, cl);
    }
  else if(cmd == "volmatch")
    {
    volmatch(param, cl);
    }
  else if(cmd == "voliter")
    {
    voliter(param, cl);
    }
  else
    {
    std::cerr << "Unknown command " << cmd << std::endl;
    return -1;
    }
  }
  catch(std::exception &exc)
  {
    std::cerr << "ERROR: exception thrown in the code:" << std::endl;
    std::cerr << exc.what() << std::endl;
    return -1;
  }

  return 0;
}


