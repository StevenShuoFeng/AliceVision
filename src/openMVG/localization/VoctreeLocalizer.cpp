#include "VoctreeLocalizer.hpp"

#include <openMVG/sfm/sfm_data_io.hpp>
#include <openMVG/sfm/pipelines/sfm_robust_model_estimation.hpp>
#include <openMVG/sfm/sfm_data_BA_ceres.hpp>
#include <openMVG/features/io_regions_type.hpp>
#include <openMVG/features/svgVisualization.hpp>
#ifdef HAVE_CCTAG
#include <openMVG/features/cctag/SIFT_CCTAG_describer.hpp>
#endif
#include <nonFree/sift/SIFT_float_describer.hpp>
#include <openMVG/matching/regions_matcher.hpp>
#include <openMVG/matching_image_collection/Matcher.hpp>
#include <openMVG/matching/matcher_kdtree_flann.hpp>
#include <openMVG/matching_image_collection/F_ACRobust.hpp>
#include <openMVG/numeric/numeric.h>
#include <openMVG/robust_estimation/guided_matching.hpp>
#include <openMVG/logger.hpp>

#include <third_party/progress/progress.hpp>
//#include <cereal/archives/json.hpp>

#include <boost/filesystem.hpp>

#include <algorithm>
#include <chrono>

namespace openMVG {
namespace localization {

std::ostream& operator<<( std::ostream& os, const voctree::Document &doc )	
{
  os << "[ ";
  for( const voctree::Word &w : doc )
  {
          os << w << ", ";
  }
  os << "];\n";
  return os;
}

std::ostream& operator<<(std::ostream& os, VoctreeLocalizer::Algorithm a)
{
  switch(a)
  {
  case VoctreeLocalizer::Algorithm::FirstBest: os << "FirstBest";
    break;
  case VoctreeLocalizer::Algorithm::BestResult: os << "BestResult";
    break;
  case VoctreeLocalizer::Algorithm::AllResults: os << "AllResults";
    break;
  case VoctreeLocalizer::Algorithm::Cluster: os << "Cluster";
    break;
  default: 
    os << "Unknown algorithm!";
    throw std::invalid_argument("Unrecognized algorithm!");
  }
  return os;
}

std::istream& operator>>(std::istream &in, VoctreeLocalizer::Algorithm &a)
{
  POPART_COUT("operator>>");
  int i;
  in >> i;
  a = static_cast<VoctreeLocalizer::Algorithm> (i);
  return in;
}	

VoctreeLocalizer::Algorithm VoctreeLocalizer::initFromString(const std::string &value)
{
  if(value=="FirstBest")
    return VoctreeLocalizer::Algorithm::FirstBest;
  else if(value=="AllResults")
    return VoctreeLocalizer::Algorithm::AllResults;
  else if(value=="BestResult")
    throw std::invalid_argument("BestResult not yet implemented");
  else if(value=="Cluster")
    throw std::invalid_argument("Cluster not yet implemented");
  else
    throw std::invalid_argument("Unrecognized algorithm \"" + value + "\"!");
}


VoctreeLocalizer::VoctreeLocalizer(const std::string &sfmFilePath,
                                   const std::string &descriptorsFolder,
                                   const std::string &vocTreeFilepath,
                                   const std::string &weightsFilepath
#ifdef HAVE_CCTAG
                                   , bool useSIFT_CCTAG
#endif
                                  ) : ILocalizer() , _frameBuffer(5)
{
  using namespace openMVG::features;
  // init the feature extractor
#ifdef HAVE_CCTAG
  if(useSIFT_CCTAG)
  {
    POPART_COUT("SIFT_CCTAG_Image_describer");
    _image_describer = new features::SIFT_CCTAG_Image_describer();  
  }
  else
  {
#if USE_SIFT_FLOAT
    POPART_COUT("SIFT_float_describer");
    _image_describer = new features::SIFT_float_describer();
#else
    POPART_COUT("SIFT_Image_describer");
    _image_describer = new features::SIFT_Image_describer();
#endif
  }
#else
#if USE_SIFT_FLOAT
    POPART_COUT("SIFT_float_describer");
    _image_describer = new features::SIFT_float_describer();
#else
    POPART_COUT("SIFT_Image_describer");
    _image_describer = new features::SIFT_Image_describer();
#endif
#endif
  
  // load the sfm data containing the 3D reconstruction info
  POPART_COUT("Loading SFM data...");
  if (!Load(_sfm_data, sfmFilePath, sfm::ESfM_Data::ALL)) 
  {
    POPART_CERR("The input SfM_Data file " << sfmFilePath << " cannot be read!");
    POPART_CERR("\n\nIf the error says \"JSON Parsing failed - provided NVP not found\" "
            "it's likely that you have to convert your sfm_data to a recent version supporting "
            "polymorphic Views. You can run the python script convertSfmData.py to update an existing sfmdata.");
    _isInit = false;
    return;
  }

  POPART_COUT("SfM data loaded from " << sfmFilePath << " containing: ");
  POPART_COUT("\tnumber of views      : " << _sfm_data.GetViews().size());
  POPART_COUT("\tnumber of poses      : " << _sfm_data.GetPoses().size());
  POPART_COUT("\tnumber of points     : " << _sfm_data.GetLandmarks().size());
  POPART_COUT("\tnumber of intrinsics : " << _sfm_data.GetIntrinsics().size());

  // load the features and descriptors
  // initially we need all the feature in order to create the database
  // then we can store only those associated to 3D points
  //? can we use Feature_Provider to load the features and filter them later?

  _isInit = initDatabase(vocTreeFilepath, weightsFilepath, descriptorsFolder);
}

bool VoctreeLocalizer::localize(const std::unique_ptr<features::Regions> &genQueryRegions,
                                const std::pair<std::size_t, std::size_t> &imageSize,
                                const LocalizerParameters *param,
                                bool useInputIntrinsics,
                                cameras::Pinhole_Intrinsic_Radial_K3 &queryIntrinsics,
                                LocalizationResult & localizationResult,
                                const std::string& imagePath)
{
  const Parameters *voctreeParam = static_cast<const Parameters *>(param);
  if(!voctreeParam)
  {
    // error!
    throw std::invalid_argument("The parameters are not in the right format!!");
  }

#if USE_SIFT_FLOAT  
  features::SIFT_Float_Regions *queryRegions = dynamic_cast<features::SIFT_Float_Regions*> (genQueryRegions.get());
#else
  features::SIFT_Regions *queryRegions = dynamic_cast<features::SIFT_Regions*> (genQueryRegions.get());
#endif
  
  if(!queryRegions)
  {
    // error, the casting did not work
    throw std::invalid_argument("Error while converting the input Regions. Only SIFT regions are allowed for the voctree localizer.");
  }
  
  switch(voctreeParam->_algorithm)
  {
    case Algorithm::FirstBest:
    return localizeFirstBestResult(*queryRegions,
                                   imageSize,
                                   *voctreeParam,
                                   useInputIntrinsics,
                                   queryIntrinsics,
                                   localizationResult,
                                   imagePath);
    case Algorithm::BestResult: throw std::invalid_argument("BestResult not yet implemented");
    case Algorithm::AllResults:
    return localizeAllResults(*queryRegions,
                              imageSize,
                              *voctreeParam,
                              useInputIntrinsics,
                              queryIntrinsics,
                              localizationResult,
                              imagePath);
    case Algorithm::Cluster: throw std::invalid_argument("Cluster not yet implemented");
    default: throw std::invalid_argument("Unknown algorithm type");
  }
}

bool VoctreeLocalizer::localize(const image::Image<unsigned char> & imageGrey,
                                const LocalizerParameters *param,
                                bool useInputIntrinsics,
                                cameras::Pinhole_Intrinsic_Radial_K3 &queryIntrinsics,
                                LocalizationResult &localizationResult,
                                const std::string& imagePath /* = std::string() */)
{
  // A. extract descriptors and features from image
  POPART_COUT("[features]\tExtract SIFT from query image");
#if USE_SIFT_FLOAT
  std::unique_ptr<features::Regions> tmpQueryRegions(new features::SIFT_Float_Regions());
#else
  std::unique_ptr<features::Regions> tmpQueryRegions(new features::SIFT_Regions());
#endif
  auto detect_start = std::chrono::steady_clock::now();
  _image_describer->Set_configuration_preset(param->_featurePreset);
  _image_describer->Describe(imageGrey, tmpQueryRegions, nullptr);
  auto detect_end = std::chrono::steady_clock::now();
  auto detect_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(detect_end - detect_start);
  POPART_COUT("[features]\tExtract SIFT done: found " << tmpQueryRegions->RegionCount() << " features in " << detect_elapsed.count() << " [ms]");

  const std::pair<std::size_t, std::size_t> queryImageSize = std::make_pair(imageGrey.Width(), imageGrey.Height());

  // if debugging is enable save the svg image with the extracted features
  if(!param->_visualDebug.empty() && !imagePath.empty())
  {
    namespace bfs = boost::filesystem;
    saveFeatures2SVG(imagePath,
                     queryImageSize,
                     tmpQueryRegions->GetRegionsPositions(),
                     param->_visualDebug + "/" + bfs::path(imagePath).stem().string() + ".svg");
  }

  return localize(tmpQueryRegions,
                  queryImageSize,
                  param,
                  useInputIntrinsics,
                  queryIntrinsics,
                  localizationResult,
                  imagePath);
}

//@fixme deprecated.. now inside initDatabase

bool VoctreeLocalizer::loadReconstructionDescriptors(const sfm::SfM_Data & sfm_data,
                                                     const std::string & feat_directory)
{
  C_Progress_display my_progress_bar(sfm_data.GetViews().size(),
                                     std::cout, "\n- Regions Loading -\n");

  POPART_COUT("Build observations per view");
  // Build observations per view
  std::map<IndexT, std::vector<FeatureInImage> > observationsPerView;
  for(auto landmarkValue : sfm_data.structure)
  {
    IndexT trackId = landmarkValue.first;
    sfm::Landmark& landmark = landmarkValue.second;
    for(auto obs : landmark.obs)
    {
      const IndexT viewId = obs.first;
      const sfm::Observation& obs2d = obs.second;
      observationsPerView[viewId].push_back(FeatureInImage(obs2d.id_feat, trackId));
    }
  }
  for(auto featuresInImage : observationsPerView)
  {
    std::sort(featuresInImage.second.begin(), featuresInImage.second.end());
  }

  POPART_COUT("Load Features and Descriptors per view");
  // Read for each view the corresponding regions and store them
  for(sfm::Views::const_iterator iter = sfm_data.GetViews().begin();
          iter != sfm_data.GetViews().end(); ++iter, ++my_progress_bar)
  {
    const IndexT id_view = iter->second->id_view;
    Reconstructed_RegionsT& reconstructedRegion = _regions_per_view[id_view];

    const std::string sImageName = stlplus::create_filespec(sfm_data.s_root_path, iter->second.get()->s_Img_path);
    const std::string basename = stlplus::basename_part(sImageName);
    const std::string featFilepath = stlplus::create_filespec(feat_directory, basename, ".feat");
    const std::string descFilepath = stlplus::create_filespec(feat_directory, basename, ".desc");
    //    std::cout << "Feat: " << featFilepath << std::endl;
    //    std::cout << "Desc: " << descFilepath << std::endl;

    if(!reconstructedRegion._regions.Load(featFilepath, descFilepath))
    {
      POPART_CERR("Invalid regions files for the view: " << sImageName);
      return false;
    }

    // Filter descriptors to keep only the 3D reconstructed points
    reconstructedRegion.filterRegions(observationsPerView[id_view]);
  }
  return true;
}

/**
 * @brief Initialize the database...
 */
bool VoctreeLocalizer::initDatabase(const std::string & vocTreeFilepath,
                                    const std::string & weightsFilepath,
                                    const std::string & feat_directory)
{

  bool withWeights = !weightsFilepath.empty();

  // Load vocabulary tree
  POPART_COUT("Loading vocabulary tree...");

  _voctree.load(vocTreeFilepath);
  POPART_COUT("tree loaded with" << endl << "\t" << _voctree.levels() << " levels" 
          << endl << "\t" << _voctree.splits() << " branching factor");

  POPART_COUT("Creating the database...");
  // Add each object (document) to the database
  _database = voctree::Database(_voctree.words());
  if(withWeights)
  {
    POPART_COUT("Loading weights...");
    _database.loadWeights(weightsFilepath);
  }
  else
  {
    POPART_COUT("No weights specified, skipping...");
  }
  
  // Load the descriptors and the features related to the images
  // for every image, pass the descriptors through the vocabulary tree and
  // add its visual words to the database.
  // then only store the feature and descriptors that have a 3D point associated
  POPART_COUT("Build observations per view");
  C_Progress_display my_progress_bar(_sfm_data.GetViews().size(),
                                     std::cout, "\n- Load Features and Descriptors per view -\n");

  // Build observations per view
  std::map<IndexT, std::vector<FeatureInImage> > observationsPerView;
  for(auto landmarkValue : _sfm_data.structure)
  {
    IndexT trackId = landmarkValue.first;
    sfm::Landmark& landmark = landmarkValue.second;
    for(auto obs : landmark.obs)
    {
      const IndexT viewId = obs.first;
      const sfm::Observation& obs2d = obs.second;
      observationsPerView[viewId].emplace_back(obs2d.id_feat, trackId);
    }
  }
  for(auto featuresInImage : observationsPerView)
  {
    std::sort(featuresInImage.second.begin(), featuresInImage.second.end());
  }

  // Read for each view the corresponding Regions and store them
  for(const auto &iter : _sfm_data.GetViews())
  {
    const std::shared_ptr<sfm::View> currView = iter.second;
    const IndexT id_view = currView->id_view;
    Reconstructed_RegionsT& currRecoRegions = _regions_per_view[id_view];

    const std::string sImageName = stlplus::create_filespec(_sfm_data.s_root_path, currView.get()->s_Img_path);
    std::string featFilepath = stlplus::create_filespec(feat_directory, std::to_string(iter.first), ".feat");
    std::string descFilepath = stlplus::create_filespec(feat_directory, std::to_string(iter.first), ".desc");

    if(!(stlplus::is_file(featFilepath) && stlplus::is_file(descFilepath)))
    {
      // legacy compatibility, if the features are not named using the UID convention
      // let's try with the old-fashion naming convention
      const std::string basename = stlplus::basename_part(sImageName);
      featFilepath = stlplus::create_filespec(feat_directory, basename, ".feat");
      descFilepath = stlplus::create_filespec(feat_directory, basename, ".desc");
      if(!(stlplus::is_file(featFilepath) && stlplus::is_file(descFilepath)))
      {
        POPART_CERR("Cannot find the features for image " << sImageName 
                << " neither using the UID naming convention nor the image name based convention");
        return false;
      }
    }

    if(!currRecoRegions._regions.Load(featFilepath, descFilepath))
    {
      std::cerr << "Invalid regions files for the view: " << sImageName << std::endl;
      return false;
    }
    
    voctree::SparseHistogram histo;
    std::vector<voctree::Word> words = _voctree.quantize(currRecoRegions._regions.Descriptors());
    voctree::computeSparseHistogram(words, histo);
    _database.insert(id_view, histo);

    // Filter descriptors to keep only the 3D reconstructed points
    currRecoRegions.filterRegions(observationsPerView[id_view]);
    ++my_progress_bar;
  }
  
  return true;
}

bool VoctreeLocalizer::localizeFirstBestResult(const features::SIFT_Regions &queryRegions,
                                               const std::pair<std::size_t, std::size_t> queryImageSize,
                                               const Parameters &param,
                                               bool useInputIntrinsics,
                                               cameras::Pinhole_Intrinsic_Radial_K3 &queryIntrinsics,
                                               LocalizationResult &localizationResult,
                                               const std::string& imagePath /*= std::string()*/)
{
  // A. Find the (visually) similar images in the database 
  POPART_COUT("[database]\tRequest closest images from voctree");
  // pass the descriptors through the vocabulary tree to get the visual words
  // associated to each feature
  std::vector<voctree::Word> requestImageWords = _voctree.quantize(queryRegions.Descriptors());
  
  // Request closest images from voctree
  std::vector<voctree::DocMatch> matchedImages;
  _database.find(requestImageWords, param._numResults, matchedImages);
  
//  // just debugging bla bla
//  // for each similar image found print score and number of features
//  for(const voctree::DocMatch & currMatch : matchedImages )
//  {
//    // get the corresponding index of the view
//    const IndexT matchedViewIndex = currMatch.id;
//    // get the view handle
//    const std::shared_ptr<sfm::View> matchedView = _sfm_data.views[matchedViewIndex];
//    POPART_COUT( "[database]\t\t match " << matchedView->s_Img_path 
//            << " [docid: "<< currMatch.id << "]"
//            << " with score " << currMatch.score 
//            << " and it has "  << _regions_per_view[matchedViewIndex]._regions.RegionCount() 
//            << " features with 3D points");
//  }

  POPART_COUT("[matching]\tBuilding the matcher");
  matching::RegionsMatcherT<MatcherT> matcher(queryRegions);
  
  sfm::Image_Localizer_Match_Data resectionData;
  std::vector<pair<IndexT, IndexT> > associationIDs;
  geometry::Pose3 pose;
 
  // B. for each found similar image, try to find the correspondences between the 
  // query image and the similar image
  for(const voctree::DocMatch& matchedImage : matchedImages)
  {
    // minimum number of points that allows a reliable 3D reconstruction
    const size_t minNum3DPoints = 5;
    
    // the view index of the current matched image
    const IndexT matchedViewIndex = matchedImage.id;
    // the handler to the current view
    const std::shared_ptr<sfm::View> matchedView = _sfm_data.views[matchedViewIndex];
    
    // safeguard: we should match the query image with an image that has at least
    // some 3D points visible --> if it has 0 3d points it is likely that it is an
    // image of the dataset that was not reconstructed
    if(_regions_per_view[matchedViewIndex]._regions.RegionCount() < minNum3DPoints)
    {
      POPART_COUT("[matching]\tSkipping matching with " << matchedView->s_Img_path << " as it has too few visible 3D points (" << _regions_per_view[matchedViewIndex]._regions.RegionCount() << ")");
      continue;
    }
    else
    {
      POPART_COUT("[matching]\tTrying to match the query image with " << matchedView->s_Img_path);
    }
    
    // its associated reconstructed regions
    const Reconstructed_RegionsT& matchedRegions = _regions_per_view[matchedViewIndex];
    // its associated intrinsics
    // this is just ugly!
    const cameras::IntrinsicBase *matchedIntrinsicsBase = _sfm_data.intrinsics[matchedView->id_intrinsic].get();
    if ( !isPinhole(matchedIntrinsicsBase->getType()) )
    {
      //@fixme maybe better to throw something here
      POPART_CERR("Only Pinhole cameras are supported!");
      return false;
    }
    const cameras::Pinhole_Intrinsic *matchedIntrinsics = (const cameras::Pinhole_Intrinsic*)(matchedIntrinsicsBase);
     
    std::vector<matching::IndMatch> vec_featureMatches;
    bool matchWorked = robustMatching( matcher, 
                                      // pass the input intrinsic if they are valid, null otherwise
                                      (useInputIntrinsics) ? &queryIntrinsics : nullptr,
                                      matchedRegions._regions,
                                      matchedIntrinsics,
                                      param._fDistRatio,
                                      param._matchingError,
                                      param._useGuidedMatching,
                                      queryImageSize,
                                      std::make_pair(matchedView->ui_width, matchedView->ui_height), 
                                      vec_featureMatches,
                                      param._matchingEstimator);
    if (!matchWorked)
    {
      POPART_COUT("[matching]\tMatching with " << matchedView->s_Img_path << " failed! Skipping image");
      continue;
    }
    else
    {
      POPART_COUT("[matching]\tFound " << vec_featureMatches.size() << " matches");
    }
    assert(vec_featureMatches.size()>0);
    
    if(!param._visualDebug.empty() && !imagePath.empty())
    {
      namespace bfs = boost::filesystem;
      const sfm::View *mview = _sfm_data.GetViews().at(matchedViewIndex).get();
      const std::string queryimage = bfs::path(imagePath).stem().string();
      const std::string matchedImage = bfs::path(mview->s_Img_path).stem().string();
      const std::string matchedPath = (bfs::path(_sfm_data.s_root_path) /  bfs::path(mview->s_Img_path)).string();
      
      features::saveMatches2SVG(imagePath,
                      queryImageSize,
                      queryRegions.GetRegionsPositions(),
                      matchedPath,
                      std::make_pair(mview->ui_width, mview->ui_height),
                      _regions_per_view[matchedViewIndex]._regions.GetRegionsPositions(),
                      vec_featureMatches,
                      param._visualDebug + "/" + queryimage + "_" + matchedImage + ".svg"); 
    }
    
    // C. recover the 2D-3D associations from the matches 
    // Each matched feature in the current similar image is associated to a 3D point,
    // hence we can recover the 2D-3D associations to estimate the pose
    // Prepare data for resection
    resectionData = sfm::Image_Localizer_Match_Data();
    resectionData.pt2D = Mat2X(2, vec_featureMatches.size());
    resectionData.pt3D = Mat3X(3, vec_featureMatches.size());
    associationIDs.clear();
    associationIDs.reserve(vec_featureMatches.size());

    // Get the 3D points associated to each matched feature
    std::size_t index = 0;
    for(const matching::IndMatch& featureMatch : vec_featureMatches)
    {
      assert(vec_featureMatches.size()>index);
      // the ID of the 3D point
      const IndexT trackId3D = matchedRegions._associated3dPoint[featureMatch._j];

      // prepare data for resectioning
      resectionData.pt3D.col(index) = _sfm_data.GetLandmarks().at(trackId3D).X;

      const Vec2 feat = queryRegions.GetRegionPosition(featureMatch._i);
      resectionData.pt2D.col(index) = feat;
      
      associationIDs.emplace_back(trackId3D, featureMatch._i);

      ++index;
    }
    // estimate the pose
    // Do the resectioning: compute the camera pose.
    resectionData.error_max = param._errorMax;
    POPART_COUT("[poseEstimation]\tEstimating camera pose...");
    bool bResection = sfm::SfM_Localizer::Localize(queryImageSize,
                                                   // pass the input intrinsic if they are valid, null otherwise
                                                   (useInputIntrinsics) ? &queryIntrinsics : nullptr,
                                                   resectionData,
                                                   pose,
                                                   param._resectionEstimator);

    if(!bResection)
    {
      POPART_COUT("[poseEstimation]\tResection FAILED");
      continue;
    }
    POPART_COUT("[poseEstimation]\tResection SUCCEDED");
       
    POPART_COUT("R est\n" << pose.rotation());
    POPART_COUT("t est\n" << pose.translation());

    // if we didn't use the provided intrinsics, estimate K from the projection
    // matrix estimated by the localizer and initialize the queryIntrinsics with
    // it and the image size. This will provide a first guess for the refine function
    if(!useInputIntrinsics)
    {
      // Decompose P matrix
      Mat3 K_, R_;
      Vec3 t_;
      // Decompose the projection matrix  to get K, R and t using 
      // RQ decomposition
      KRt_From_P(resectionData.projection_matrix, &K_, &R_, &t_);
      POPART_COUT("K estimated\n" << K_);
      queryIntrinsics.setK(K_);
      queryIntrinsics.setWidth(queryImageSize.first);
      queryIntrinsics.setHeight(queryImageSize.second);
    }

    // D. refine the estimated pose
    POPART_COUT("[poseEstimation]\tRefining estimated pose");
    bool refineStatus = sfm::SfM_Localizer::RefinePose(&queryIntrinsics, 
                                                       pose, 
                                                       resectionData, 
                                                       true /*b_refine_pose*/, 
                                                       param._refineIntrinsics /*b_refine_intrinsic*/);
    if(!refineStatus)
      POPART_COUT("[poseEstimation]\tRefine pose could not improve the estimation of the camera pose.");
    
    {
      // just temporary code to evaluate the estimated pose @todo remove it
      const geometry::Pose3 &referencePose = _sfm_data.poses[matchedViewIndex];
      POPART_COUT("R refined\n" << pose.rotation());
      POPART_COUT("t refined\n" << pose.translation());
      POPART_COUT("K refined\n" << queryIntrinsics.K());
      POPART_COUT("R_gt\n" << referencePose.rotation());
      POPART_COUT("t_gt\n" << referencePose.translation());
      POPART_COUT("angular difference: " << R2D(getRotationMagnitude(pose.rotation()*referencePose.rotation().inverse())) << "deg");
      POPART_COUT("center difference: " << (pose.center()-referencePose.center()).norm());
      POPART_COUT("err = [err; " << R2D(getRotationMagnitude(pose.rotation()*referencePose.rotation().inverse())) << ", "<< (pose.center()-referencePose.center()).norm() << "];");
    }
    localizationResult = LocalizationResult(resectionData, associationIDs, pose, queryIntrinsics, matchedImages, true);
    break;
  }
  //@todo deal with unsuccesful case...
  return localizationResult.isValid();
  
 } 

bool VoctreeLocalizer::localizeAllResults(const features::SIFT_Regions &queryRegions,
                                          const std::pair<std::size_t, std::size_t> queryImageSize,
                                          const Parameters &param,
                                          bool useInputIntrinsics,
                                          cameras::Pinhole_Intrinsic_Radial_K3 &queryIntrinsics,
                                          LocalizationResult &localizationResult,
                                          const std::string& imagePath)
{
  
  sfm::Image_Localizer_Match_Data resectionData;
  std::map< std::pair<IndexT, IndexT>, std::size_t > occurences;
  
  // get all the association from the database images
  std::vector<voctree::DocMatch> matchedImages;
  getAllAssociations(queryRegions,
                     queryImageSize,
                     param,
                     useInputIntrinsics,
                     queryIntrinsics,
                     occurences,
                     resectionData.pt2D,
                     resectionData.pt3D,
                     matchedImages,
                     imagePath);

  const std::size_t numCollectedPts = occurences.size();
  std::vector<pair<IndexT, IndexT> > associationIDs;
  associationIDs.reserve(numCollectedPts);

  for(const auto &ass : occurences)
  {
    // recopy the associations IDs in the vector
    associationIDs.push_back(ass.first);
  }
  
  assert(associationIDs.size() == numCollectedPts);
  assert(resectionData.pt2D.cols() == numCollectedPts);
  assert(resectionData.pt3D.cols() == numCollectedPts);

  geometry::Pose3 pose;
  
  // estimate the pose
  // Do the resectioning: compute the camera pose.
  resectionData.error_max = param._errorMax;
  POPART_COUT("[poseEstimation]\tEstimating camera pose...");
  const bool bResection = sfm::SfM_Localizer::Localize(queryImageSize,
                                                      // pass the input intrinsic if they are valid, null otherwise
                                                      (useInputIntrinsics) ? &queryIntrinsics : nullptr,
                                                      resectionData,
                                                      pose,
                                                      param._resectionEstimator);

  if(!bResection)
  {
    POPART_COUT("[poseEstimation]\tResection FAILED");
    if(!param._visualDebug.empty() && !imagePath.empty())
    {
      namespace bfs = boost::filesystem;
      features::saveFeatures2SVG(imagePath, 
                       queryImageSize, 
                         resectionData.pt2D,
                         param._visualDebug + "/" + bfs::path(imagePath).stem().string() + ".associations.svg");
    }
    localizationResult = LocalizationResult();
    return localizationResult.isValid();
  }
  POPART_COUT("[poseEstimation]\tResection SUCCEDED");

  POPART_COUT("R est\n" << pose.rotation());
  POPART_COUT("t est\n" << pose.translation());

  // if we didn't use the provided intrinsics, estimate K from the projection
  // matrix estimated by the localizer and initialize the queryIntrinsics with
  // it and the image size. This will provide a first guess for the refine function
  if(!useInputIntrinsics)
  {
    // Decompose P matrix
    Mat3 K_, R_;
    Vec3 t_;
    // Decompose the projection matrix  to get K, R and t using 
    // RQ decomposition
    KRt_From_P(resectionData.projection_matrix, &K_, &R_, &t_);
    queryIntrinsics.setK(K_);
    POPART_COUT("K estimated\n" << K_);
    queryIntrinsics.setWidth(queryImageSize.first);
    queryIntrinsics.setHeight(queryImageSize.second);
  }

  // E. refine the estimated pose
  POPART_COUT("[poseEstimation]\tRefining estimated pose");
  bool refineStatus = sfm::SfM_Localizer::RefinePose(&queryIntrinsics,
                                                     pose,
                                                     resectionData,
                                                     true /*b_refine_pose*/,
                                                     param._refineIntrinsics /*b_refine_intrinsic*/);
  if(!refineStatus)
    POPART_COUT("Refine pose could not improve the estimation of the camera pose.");

  if(!param._visualDebug.empty() && !imagePath.empty())
  {
    namespace bfs = boost::filesystem;
    features::saveFeatures2SVG(imagePath,
                     queryImageSize,
                     resectionData.pt2D,
                     param._visualDebug + "/" + bfs::path(imagePath).stem().string() + ".associations.svg",
                     &resectionData.vec_inliers);
  }

  localizationResult = LocalizationResult(resectionData, associationIDs, pose, queryIntrinsics, matchedImages, true);

  {
    // just debugging this block can be safely removed or commented out
    POPART_COUT("R refined\n" << pose.rotation());
    POPART_COUT("t refined\n" << pose.translation());
    POPART_COUT("K refined\n" << queryIntrinsics.K());

    const Mat2X residuals = localizationResult.computeInliersResiduals();

    const auto sqrErrors = (residuals.cwiseProduct(residuals)).colwise().sum();
    POPART_COUT("RMSE = " << std::sqrt(sqrErrors.mean())
                << " min = " << std::sqrt(sqrErrors.minCoeff())
                << " max = " << std::sqrt(sqrErrors.maxCoeff()));
  }
  
    
  if(param._useFrameBufferMatching)
  {
    // add everything to the buffer
    FrameData fi(localizationResult, queryRegions);
    _frameBuffer.push_back(fi);
  }

  return localizationResult.isValid();
}

void VoctreeLocalizer::getAllAssociations(const features::SIFT_Regions &queryRegions,
                                          const std::pair<std::size_t, std::size_t> imageSize,
                                          const Parameters &param,
                                          bool useInputIntrinsics,
                                          const cameras::Pinhole_Intrinsic_Radial_K3 &queryIntrinsics,
                                          std::map< std::pair<IndexT, IndexT>, std::size_t > &occurences,
                                          Mat &pt2D,
                                          Mat &pt3D,
                                          std::vector<voctree::DocMatch>& matchedImages,
                                          const std::string& imagePath) const
{
  // A. Find the (visually) similar images in the database 
  // pass the descriptors through the vocabulary tree to get the visual words
  // associated to each feature
  POPART_COUT("[database]\tRequest closest images from voctree");
  std::vector<voctree::Word> requestImageWords = _voctree.quantize(queryRegions.Descriptors());
  
  // Request closest images from voctree
  _database.find(requestImageWords, (param._numResults==0) ? (_database.size()) : (param._numResults) , matchedImages);
  
//  // just debugging bla bla
//  // for each similar image found print score and number of features
//  for(const voctree::DocMatch& currMatch : matchedImages )
//  {
//    // get the view handle
//    const std::shared_ptr<sfm::View> matchedView = _sfm_data.views[currMatch.id];
//    POPART_COUT( "[database]\t\t match " << matchedView->s_Img_path 
//            << " [docid: "<< currMatch.id << "]"
//            << " with score " << currMatch.score 
//            << " and it has "  << _regions_per_view[currMatch.id]._regions.RegionCount() 
//            << " features with 3D points");
//  }


  POPART_COUT("[matching]\tBuilding the matcher");
  matching::RegionsMatcherT<MatcherT> matcher(queryRegions);

  
  std::map< std::pair<IndexT, IndexT>, std::size_t > repeated;
  
  // B. for each found similar image, try to find the correspondences between the 
  // query image adn the similar image
  // stop when param._maxResults successful matches have been found
  std::size_t goodMatches = 0;
  for(const voctree::DocMatch& matchedImage : matchedImages)
  {
    // minimum number of points that allows a reliable 3D reconstruction
    const size_t minNum3DPoints = 5;
    
    // the handler to the current view
    const std::shared_ptr<sfm::View> matchedView = _sfm_data.views.at(matchedImage.id);
    // its associated reconstructed regions
    const Reconstructed_RegionsT& matchedRegions = _regions_per_view.at(matchedImage.id);
    
    // safeguard: we should match the query image with an image that has at least
    // some 3D points visible --> if this is not true it is likely that it is an
    // image of the dataset that was not reconstructed
    if(matchedRegions._regions.RegionCount() < minNum3DPoints)
    {
      POPART_COUT("[matching]\tSkipping matching with " << matchedView->s_Img_path << " as it has too few visible 3D points");
      continue;
    }
    POPART_COUT("[matching]\tTrying to match the query image with " << matchedView->s_Img_path);
    POPART_COUT("[matching]\tit has " << matchedRegions._regions.RegionCount() << " available features to match");
    
    // its associated intrinsics
    // this is just ugly!
    const cameras::IntrinsicBase *matchedIntrinsicsBase = _sfm_data.intrinsics.at(matchedView->id_intrinsic).get();
    if ( !isPinhole(matchedIntrinsicsBase->getType()) )
    {
      //@fixme maybe better to throw something here
      POPART_CERR("Only Pinhole cameras are supported!");
      return;
    }
    const cameras::Pinhole_Intrinsic *matchedIntrinsics = (const cameras::Pinhole_Intrinsic*)(matchedIntrinsicsBase);
     
    std::vector<matching::IndMatch> vec_featureMatches;
    bool matchWorked = robustMatching( matcher, 
                                      // pass the input intrinsic if they are valid, null otherwise
                                      (useInputIntrinsics) ? &queryIntrinsics : nullptr,
                                      matchedRegions._regions,
                                      matchedIntrinsics,
                                      param._fDistRatio,
                                      param._matchingError,
                                      param._useGuidedMatching,
                                      imageSize,
                                      std::make_pair(matchedView->ui_width, matchedView->ui_height), 
                                      vec_featureMatches,
                                      param._matchingEstimator);
    if (!matchWorked)
    {
//      POPART_COUT("[matching]\tMatching with " << matchedView->s_Img_path << " failed! Skipping image");
      continue;
    }
    else
    {
      POPART_COUT("[matching]\tFound " << vec_featureMatches.size() << " matches");
    }
    assert(vec_featureMatches.size()>0);
    
    // if debug is enable save the matches between the query image and the current matching image
    // It saves the feature matches in a folder with the same name as the query
    // image, if it does not exist it will create it. The final svg file will have
    // a name like this: queryImage_matchedImage.svg placed in the following directory:
    // param._visualDebug/queryImage/
    if(!param._visualDebug.empty() && !imagePath.empty())
    {
      namespace bfs = boost::filesystem;
      const auto &matchedViewIndex = matchedImage.id;
      const sfm::View *mview = _sfm_data.GetViews().at(matchedViewIndex).get();
      // the current query image without extension
      const auto queryImage = bfs::path(imagePath).stem();
      // the matching image without extension
      const auto matchedImage = bfs::path(mview->s_Img_path).stem();
      // the full path of the matching image
      const auto matchedPath = (bfs::path(_sfm_data.s_root_path) /  bfs::path(mview->s_Img_path)).string();

      // the directory where to save the feature matches
      const auto baseDir = bfs::path(param._visualDebug) / queryImage;
      if((!bfs::exists(baseDir)))
      {
        POPART_COUT("created " << baseDir.string());
        bfs::create_directories(baseDir);
      }
      
      // damn you, boost, what does it take to make the operator "+"?
      // the final filename for the output svg file as a composition of the query
      // image and the matched image
      auto outputName = baseDir / queryImage;
      outputName += "_";
      outputName += matchedImage;
      outputName += ".svg";

      features::saveMatches2SVG(imagePath,
                                imageSize,
                                queryRegions.GetRegionsPositions(),
                                matchedPath,
                                std::make_pair(mview->ui_width, mview->ui_height),
                                _regions_per_view.at(matchedViewIndex)._regions.GetRegionsPositions(),
                                vec_featureMatches,
                                outputName.string()); 
    }
    
    // C. recover the 2D-3D associations from the matches 
    // Each matched feature in the current similar image is associated to a 3D point
    for(const matching::IndMatch& featureMatch : vec_featureMatches)
    {
      // the ID of the 3D point
      const IndexT pt3D_id = matchedRegions._associated3dPoint[featureMatch._j];
      const IndexT pt2D_id = featureMatch._i;
      
      const auto key = std::make_pair(pt3D_id, pt2D_id);
      if(occurences.count(key))
      {
        occurences[key]++;
      }
      else
      {
        occurences[key] = 1;
      }

    }
    ++goodMatches;
    if((param._maxResults !=0) && (goodMatches == param._maxResults))
    { 
      // let's say we have enough features
      POPART_COUT("[matching]\tgot enough point from " << param._maxResults << " images");
      break;
    }
  }
  
  if(param._useFrameBufferMatching)
  {
    POPART_COUT("[matching]\tUsing frameBuffer matching: matching with the past " 
            << param._bufferSize << " frames" );
    getAssociationsFromBuffer(matcher, imageSize, param, useInputIntrinsics, queryIntrinsics, occurences);
  }
  
  const std::size_t numCollectedPts = occurences.size();
  
  {
    // just debugging statistics, this block can be safely removed
    std::size_t numOccTreated = 0;
    for(std::size_t value = 1; value < numCollectedPts; ++value)
    {
      std::size_t counter = 0;
      for(const auto &idx : occurences)
      {
        if(idx.second == value)
        {
          ++counter;
        }
      }
      POPART_COUT("[matching]\tThere are " << counter 
              <<  " associations occurred " << value << " times ("
              << 100.0*counter/(double)numCollectedPts << "%)");
      numOccTreated += counter;
      if(numOccTreated >= numCollectedPts) 
        break;
    }
  }
  
  pt2D = Mat2X(2, numCollectedPts);
  pt3D = Mat3X(3, numCollectedPts);
  
  size_t index = 0;
  for(const auto &idx : occurences)
  {
     // recopy all the points in the matching structure
    const IndexT pt3D_id = idx.first.first;
    const IndexT pt2D_id = idx.first.second;
    
//    POPART_COUT("[matching]\tAssociation [" << pt3D_id << "," << pt2D_id << "] occurred " << idx.second << " times");

    pt2D.col(index) = queryRegions.GetRegionPosition(pt2D_id);
    pt3D.col(index) = _sfm_data.GetLandmarks().at(pt3D_id).X;
     ++index;
  }
  
}

void VoctreeLocalizer::getAssociationsFromBuffer(matching::RegionsMatcherT<MatcherT> & matcher,
                                                 const std::pair<std::size_t, std::size_t> queryImageSize,
                                                 const Parameters &param,
                                                 bool useInputIntrinsics,
                                                 const cameras::Pinhole_Intrinsic_Radial_K3 &queryIntrinsics,
                                                 std::map< std::pair<IndexT, IndexT>, std::size_t > &occurences,
                                                 const std::string& imagePath) const
{
  std::size_t frameCounter = 0;
  // for all the past frames
  for(const auto& frame : _frameBuffer)
  {
    // gather the data
    const auto &frameReconstructedRegions = frame._regionsWith3D;
    const auto &frameRegions = frameReconstructedRegions._regions;
    const auto &frameIntrinsics = frame._locResult.getIntrinsics();
    const auto frameImageSize = std::make_pair(frameIntrinsics.w(), frameIntrinsics.h());
    std::vector<matching::IndMatch> vec_featureMatches;
    
    // match the query image with the current frame
    bool matchWorked = robustMatching(matcher, 
                                      // pass the input intrinsic if they are valid, null otherwise
                                      (useInputIntrinsics) ? &queryIntrinsics : nullptr,
                                      frameRegions,
                                      &frameIntrinsics,
                                      param._fDistRatio,
                                      param._matchingError,
                                      param._useGuidedMatching,
                                      queryImageSize,
                                      frameImageSize, 
                                      vec_featureMatches,
                                      param._matchingEstimator);
    if (!matchWorked)
    {
      continue;
    }

    POPART_COUT("[matching]\tFound " << vec_featureMatches.size() << " matches from frame " << frameCounter);
    assert(vec_featureMatches.size()>0);
    
    // recover the 2D-3D associations from the matches 
    // Each matched feature in the current similar image is associated to a 3D point
    std::size_t newOccurences = 0;
    for(const matching::IndMatch& featureMatch : vec_featureMatches)
    {
      // the ID of the 3D point
      const IndexT pt3D_id = frameReconstructedRegions._associated3dPoint[featureMatch._j];
      const IndexT pt2D_id = featureMatch._i;
      
      const auto key = std::make_pair(pt3D_id, pt2D_id);
      if(occurences.count(key))
      {
        occurences[key]++;
      }
      else
      {
        POPART_COUT("[matching]\tnew association found: [" << pt3D_id << "," << pt2D_id << "]");
        occurences[key] = 1;
        ++newOccurences;
      }
    }
    POPART_COUT("[matching]\tFound " << newOccurences << " new associations with the frameBufferMatching");
    ++frameCounter;
  }
}

bool VoctreeLocalizer::robustMatching(matching::RegionsMatcherT<MatcherT> & matcher, 
                                      const cameras::IntrinsicBase * queryIntrinsicsBase,   // the intrinsics of the image we are using as reference
                                      const Reconstructed_RegionsT::RegionsT & matchedRegions,
                                      const cameras::IntrinsicBase * matchedIntrinsicsBase,
                                      const float fDistRatio,
                                      const double matchingError,
                                      const bool b_guided_matching,
                                      const std::pair<size_t,size_t> & imageSizeI,     // size of the first image @fixme change the API of the kernel!! 
                                      const std::pair<size_t,size_t> & imageSizeJ,     // size of the first image
                                      std::vector<matching::IndMatch> & vec_featureMatches,
                                      robust::EROBUST_ESTIMATOR estimator /*= robust::ROBUST_ESTIMATOR_ACRANSAC*/) const
{
  // get the intrinsics of the query camera
  if ((queryIntrinsicsBase != nullptr) && !isPinhole(queryIntrinsicsBase->getType()))
  {
    //@fixme maybe better to throw something here
    POPART_CERR("[matching]\tOnly Pinhole cameras are supported!");
    return false;
  }
  const cameras::Pinhole_Intrinsic *queryIntrinsics = (const cameras::Pinhole_Intrinsic*)(queryIntrinsicsBase);
  
  // get the intrinsics of the matched view
  if ((matchedIntrinsicsBase != nullptr) &&  !isPinhole(matchedIntrinsicsBase->getType()) )
  {
    //@fixme maybe better to throw something here
    POPART_CERR("[matching]\tOnly Pinhole cameras are supported!");
    return false;
  }
  const cameras::Pinhole_Intrinsic *matchedIntrinsics = (const cameras::Pinhole_Intrinsic*)(matchedIntrinsicsBase);
  
  const bool canBeUndistorted = (queryIntrinsicsBase != nullptr) && (matchedIntrinsicsBase != nullptr);
    
  // A. Putative Features Matching
  bool matchWorked = matcher.Match(fDistRatio, matchedRegions, vec_featureMatches);
  if (!matchWorked)
  {
    POPART_COUT("[matching]\tPutative matching failed.");
    return false;
  }
  assert(vec_featureMatches.size()>0);
  // prepare the data for geometric filtering: for each matched pair of features,
  // store them in two matrices
  Mat featuresI(2, vec_featureMatches.size());
  Mat featuresJ(2, vec_featureMatches.size());
  // fill the matrices with the features according to vec_featureMatches
  for(int i = 0; i < vec_featureMatches.size(); ++i)
  {
    const matching::IndMatch& match = vec_featureMatches[i];
    const Vec2 &queryPoint = matcher.getDatabaseRegions()->GetRegionPosition(match._i);
    const Vec2 &matchedPoint = matchedRegions.GetRegionPosition(match._j);
    
    if(canBeUndistorted)
    {
      // undistort the points for the query image
      featuresI.col(i) = queryIntrinsics->get_ud_pixel(queryPoint);
      // undistort the points for the query image
      featuresJ.col(i) = matchedIntrinsics->get_ud_pixel(matchedPoint);
    }
    else
    {
      featuresI.col(i) = queryPoint;
      featuresJ.col(i) = matchedPoint;
    }
  }
  // perform the geometric filtering
  matching_image_collection::GeometricFilter_FMatrix_AC geometricFilter(matchingError, 5000);
  std::vector<size_t> vec_matchingInliers;
  bool valid = geometricFilter.Robust_estimation(featuresI, // points of the query image
                                                 featuresJ, // points of the matched image
                                                 imageSizeI,
                                                 imageSizeJ,
                                                 vec_matchingInliers,
                                                 estimator);
  if(!valid)
  {
    POPART_COUT("[matching]\tGeometric validation failed.");
    return false;
  }
  if(!b_guided_matching)
  {
    // prepare to output vec_featureMatches
    // the indices stored in vec_matchingInliers refer to featuresI, now we need
    // to recover the original indices wrt matchedRegions and queryRegions and fill 
    // a temporary vector.
    std::vector<matching::IndMatch> vec_robustFeatureMatches;
    vec_robustFeatureMatches.reserve(vec_matchingInliers.size());
    for(const size_t idx : vec_matchingInliers)
    {
      // use the index stored in vec_matchingInliers to get the indices of the 
      // original matching features and store them in vec_robustFeatureMatches
      vec_robustFeatureMatches.emplace_back(vec_featureMatches[idx]);
    }
    // just swap the vector so the output is ready
    std::swap(vec_robustFeatureMatches, vec_featureMatches);
  }
  else
  {
    // Use the Fundamental Matrix estimated by the robust estimation to
    // perform guided matching.
    // So we ignore the previous matches and recompute all matches.
    vec_featureMatches.clear();
    geometry_aware::GuidedMatching<
            Mat3,
            openMVG::fundamental::kernel::EpipolarDistanceError>(
            geometricFilter.m_F,
            queryIntrinsicsBase, // cameras::IntrinsicBase of the matched image
            *matcher.getDatabaseRegions(), // features::Regions
            matchedIntrinsicsBase, // cameras::IntrinsicBase of the query image
            matchedRegions, // features::Regions
            Square(geometricFilter.m_dPrecision_robust),
            Square(fDistRatio),
            vec_featureMatches); // output
  }
  return true;
}

bool VoctreeLocalizer::localizeRig(const std::vector<image::Image<unsigned char> > & vec_imageGrey,
                             const LocalizerParameters *parameters,
                             std::vector<cameras::Pinhole_Intrinsic_Radial_K3 > &vec_queryIntrinsics,
                             const std::vector<geometry::Pose3 > &vec_subPoses,
                             geometry::Pose3 rigPose)
{
  throw std::runtime_error("localizeRig is not yet supported for voctree_localizer");
}

} // localization
} // openMVG
