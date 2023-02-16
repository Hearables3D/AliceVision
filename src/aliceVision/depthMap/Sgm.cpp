// This file is part of the AliceVision project.
// Copyright (c) 2017 AliceVision contributors.
// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "Sgm.hpp"

#include <aliceVision/system/Logger.hpp>
#include <aliceVision/mvsUtils/fileIO.hpp>
#include <aliceVision/depthMap/depthMapUtils.hpp>
#include <aliceVision/depthMap/volumeIO.hpp>
#include <aliceVision/depthMap/cuda/host/utils.hpp>
#include <aliceVision/depthMap/cuda/host/DeviceCache.hpp>
#include <aliceVision/depthMap/cuda/planeSweeping/deviceDepthSimilarityMap.hpp>
#include <aliceVision/depthMap/cuda/planeSweeping/deviceSimilarityVolume.hpp>

#include <iostream>
#include <sstream>

namespace aliceVision {
namespace depthMap {

Sgm::Sgm(const mvsUtils::MultiViewParams& mp, 
         const mvsUtils::TileParams& tileParams, 
         const SgmParams& sgmParams,
         cudaStream_t stream)
    : _mp(mp)
    , _tileParams(tileParams)
    , _sgmParams(sgmParams)
    , _stream(stream)
{
    // get tile maximum dimensions
    const int downscale = _sgmParams.scale * _sgmParams.stepXY;
    const int maxTileWidth  = divideRoundUp(tileParams.bufferWidth , downscale);
    const int maxTileHeight = divideRoundUp(tileParams.bufferHeight, downscale);

    // allocate depth list in device memory
    {
        const CudaSize<2> depthsDim(_sgmParams.maxDepths, 1);
        _depths_hmh.allocate(depthsDim);
        _depths_dmp.allocate(depthsDim);
    }

    // allocate depth/sim map in device memory
    _depthSimMap_dmp.allocate(CudaSize<2>(maxTileWidth, maxTileHeight));

    // allocate normal map in device memory
    if(_sgmParams.computeNormalMap)
        _normalMap_dmp.allocate(CudaSize<2>(maxTileWidth, maxTileHeight));

    // allocate similarity volumes in device memory
    {
        const CudaSize<3> volDim(maxTileWidth, maxTileHeight, _sgmParams.maxDepths);

        _volumeBestSim_dmp.allocate(volDim);
        _volumeSecBestSim_dmp.allocate(volDim);
    }

    // allocate similarity volume optimization buffers
    if(sgmParams.doSgmOptimizeVolume)
    {
        const size_t maxTileSide = std::max(maxTileWidth, maxTileHeight);
        _volumeSliceAccA_dmp.allocate(CudaSize<2>(maxTileSide, _sgmParams.maxDepths));
        _volumeSliceAccB_dmp.allocate(CudaSize<2>(maxTileSide, _sgmParams.maxDepths));
        _volumeAxisAcc_dmp.allocate(CudaSize<2>(maxTileSide, 1));
    }
}

double Sgm::getDeviceMemoryConsumption() const
{
    size_t bytes = 0;

    bytes += _depths_dmp.getBytesPadded();
    bytes += _depthSimMap_dmp.getBytesPadded();
    bytes += _volumeBestSim_dmp.getBytesPadded();
    bytes += _volumeSecBestSim_dmp.getBytesPadded();

    if(_sgmParams.computeNormalMap)
        bytes += _normalMap_dmp.getBytesPadded();

    if(_sgmParams.doSgmOptimizeVolume)
    {
        bytes += _volumeSliceAccA_dmp.getBytesPadded();
        bytes += _volumeSliceAccB_dmp.getBytesPadded();
        bytes += _volumeAxisAcc_dmp.getBytesPadded();
    }

    return (double(bytes) / (1024.0 * 1024.0));
}

double Sgm::getDeviceMemoryConsumptionUnpadded() const
{
    size_t bytes = 0;

    bytes += _depths_dmp.getBytesUnpadded();
    bytes += _depthSimMap_dmp.getBytesUnpadded();
    bytes += _volumeBestSim_dmp.getBytesUnpadded();
    bytes += _volumeSecBestSim_dmp.getBytesUnpadded();

    if(_sgmParams.computeNormalMap)
        bytes += _normalMap_dmp.getBytesUnpadded();

    if(_sgmParams.doSgmOptimizeVolume)
    {
        bytes += _volumeSliceAccA_dmp.getBytesUnpadded();
        bytes += _volumeSliceAccB_dmp.getBytesUnpadded();
        bytes += _volumeAxisAcc_dmp.getBytesUnpadded();
    }

    return (double(bytes) / (1024.0 * 1024.0));
}

void Sgm::sgmRc(const Tile& tile, const SgmDepthList& tileDepthList)
{
    const IndexT viewId = _mp.getViewId(tile.rc);

    ALICEVISION_LOG_INFO(tile << "SGM depth/sim map of view id: " << viewId << ", rc: " << tile.rc << " (" << (tile.rc + 1) << " / " << _mp.ncams << ").");

    // check SGM depth list and T cameras
    if(tile.sgmTCams.empty() || tileDepthList.getDepths().empty())
        ALICEVISION_THROW_ERROR(tile << "Cannot compute Semi-Global Matching, no depths or no T cameras (viewId: " << viewId << ").");
    
    // copy rc depth data in page-locked host memory
    for(int i = 0; i < tileDepthList.getDepths().size(); ++i)
        _depths_hmh(i, 0) = tileDepthList.getDepths()[i];

    // copy rc depth data in device memory
    _depths_dmp.copyFrom(_depths_hmh, _stream);

    // compute best sim and second best sim volumes
    computeSimilarityVolumes(tile, tileDepthList);

    // export intermediate volume information (if requested by user)
    exportVolumeInformation(tile, tileDepthList, _volumeSecBestSim_dmp, "beforeFiltering");

    // this is here for experimental purposes
    // to show how SGGC work on non optimized depthmaps
    // it must equals to true in normal case
    if(_sgmParams.doSgmOptimizeVolume)                      
    {
        optimizeSimilarityVolume(tile, tileDepthList);
    }
    else
    {
        // best sim volume is normally reuse to put optimized similarity
        _volumeBestSim_dmp.copyFrom(_volumeSecBestSim_dmp, _stream);
    }

    // export intermediate volume information (if requested by user)
    exportVolumeInformation(tile, tileDepthList, _volumeBestSim_dmp, "afterFiltering");

    // retrieve best depth
    retrieveBestDepth(tile, tileDepthList);

    // export intermediate depth/sim map (if requested by user)
    if(_sgmParams.exportIntermediateDepthSimMaps)
    {
        writeDepthSimMap(tile.rc, _mp, _tileParams, tile.roi, _depthSimMap_dmp, _sgmParams.scale, _sgmParams.stepXY, "_sgm");
    }

    // compute normal map from depth/sim map if needed
    if(_sgmParams.computeNormalMap)
    {
        // downscale the region of interest
        const ROI downscaledRoi = downscaleROI(tile.roi, _sgmParams.scale * _sgmParams.stepXY);

        // get R device camera from cache
        DeviceCache& deviceCache = DeviceCache::getInstance();
        const DeviceCamera& rcDeviceCamera = deviceCache.requestCamera(tile.rc, _sgmParams.scale, _mp);

        ALICEVISION_LOG_INFO(tile << "SGM normal map of view id: " << viewId << ", rc: " << tile.rc << " (" << (tile.rc + 1) << " / " << _mp.ncams << ").");
        cuda_depthSimMapComputeNormal(_normalMap_dmp, _depthSimMap_dmp, rcDeviceCamera, _sgmParams, downscaledRoi, _stream);

        writeDeviceImage(_normalMap_dmp, getFileNameFromIndex(_mp, tile.rc, mvsUtils::EFileType::depthMap, _sgmParams.scale, "Normal", tile.roi.x.begin, tile.roi.y.begin));
    }

    ALICEVISION_LOG_INFO(tile << "SGM depth/sim map done.");
}

void Sgm::computeSimilarityVolumes(const Tile& tile, const SgmDepthList& tileDepthList)
{
    ALICEVISION_LOG_INFO(tile << "SGM Compute similarity volume.");

    // downscale the region of interest
    const ROI downscaledRoi = downscaleROI(tile.roi, _sgmParams.scale * _sgmParams.stepXY);

    // initialize the two similarity volumes at 255
    cuda_volumeInitialize(_volumeBestSim_dmp, 255.f, _stream);
    cuda_volumeInitialize(_volumeSecBestSim_dmp, 255.f, _stream);
  
    // get device cache instance
    DeviceCache& deviceCache = DeviceCache::getInstance();

    // get R device camera from cache
    const DeviceCamera& rcDeviceCamera = deviceCache.requestCamera(tile.rc, _sgmParams.scale, _mp);

    // compute similarity volume per Rc Tc
    for(std::size_t tci = 0; tci < tile.sgmTCams.size(); ++tci)
    {
        const int tc = tile.sgmTCams.at(tci);

        const int firstDepth = tileDepthList.getDepthsTcLimits()[tci].x;
        const int lastDepth  = firstDepth + tileDepthList.getDepthsTcLimits()[tci].y;

        const Range tcDepthRange(firstDepth, lastDepth);

        // get T device camera from cache
        const DeviceCamera& tcDeviceCamera = deviceCache.requestCamera(tc, _sgmParams.scale, _mp);

        ALICEVISION_LOG_DEBUG(tile << "Compute similarity volume:" << std::endl
                                   << "\t- rc: " << tile.rc << std::endl
                                   << "\t- tc: " << tc << " (" << (tci + 1) << "/" << tile.sgmTCams.size() << ")" << std::endl
                                   << "\t- rc camera device id: " << rcDeviceCamera.getDeviceCamId() << std::endl
                                   << "\t- tc camera device id: " << tcDeviceCamera.getDeviceCamId() << std::endl
                                   << "\t- tc first depth: " << firstDepth << std::endl
                                   << "\t- tc last depth: " << lastDepth << std::endl
                                   << "\t- tile range x: [" << downscaledRoi.x.begin << " - " << downscaledRoi.x.end << "]" << std::endl
                                   << "\t- tile range y: [" << downscaledRoi.y.begin << " - " << downscaledRoi.y.end << "]" << std::endl);

        cuda_volumeComputeSimilarity(_volumeBestSim_dmp, 
                                     _volumeSecBestSim_dmp, 
                                     _depths_dmp, 
                                     rcDeviceCamera, 
                                     tcDeviceCamera,
                                     _sgmParams, 
                                     tcDepthRange,
                                     downscaledRoi, 
                                     _stream);
    }

    // update second best uninitialized similarity volume values with first best similarity volume values
    // - allows to avoid the particular case with a single tc (second best volume has no valid similarity values)
    // - usefull if a tc alone contributes to the calculation of a subpart of the similarity volume
    if(_sgmParams.updateUninitializedSim) // should always be true, false for debug purposes
    {
        ALICEVISION_LOG_DEBUG(tile << "SGM Update uninitialized similarity volume values from best similarity volume.");

        cuda_volumeUpdateUninitializedSimilarity(_volumeBestSim_dmp, _volumeSecBestSim_dmp, _stream);
    }
    
    ALICEVISION_LOG_INFO(tile << "SGM Compute similarity volume done.");
}

void Sgm::optimizeSimilarityVolume(const Tile& tile, const SgmDepthList& tileDepthList)
{
    ALICEVISION_LOG_INFO(tile << "SGM Optimizing volume (filtering axes: " << _sgmParams.filteringAxes << ").");

    // downscale the region of interest
    const ROI downscaledRoi = downscaleROI(tile.roi, _sgmParams.scale * _sgmParams.stepXY);

    // get R device camera from cache
    DeviceCache& deviceCache = DeviceCache::getInstance();
    const DeviceCamera& rcDeviceCamera = deviceCache.requestCamera(tile.rc, _sgmParams.scale, _mp);
    
    cuda_volumeOptimize(_volumeBestSim_dmp,    // output volume (reuse best sim to put optimized similarity)
                        _volumeSliceAccA_dmp,  // slice A accumulation buffer pre-allocate
                        _volumeSliceAccB_dmp,  // slice B accumulation buffer pre-allocate
                        _volumeAxisAcc_dmp,    // axis accumulation buffer pre-allocate
                        _volumeSecBestSim_dmp, // input volume
                        rcDeviceCamera, 
                        _sgmParams, 
                        tileDepthList.getDepths().size(),
                        downscaledRoi,
                        _stream);

    ALICEVISION_LOG_INFO(tile << "SGM Optimizing volume done.");
}

void Sgm::retrieveBestDepth(const Tile& tile, const SgmDepthList& tileDepthList)
{
    ALICEVISION_LOG_INFO(tile << "SGM Retrieve best depth in volume.");

    // downscale the region of interest
    const ROI downscaledRoi = downscaleROI(tile.roi, _sgmParams.scale * _sgmParams.stepXY);

    // get depth range
    const Range depthRange(0, tileDepthList.getDepths().size());

    // get R device camera from cache
    DeviceCache& deviceCache = DeviceCache::getInstance();
    const DeviceCamera& rcDeviceCamera = deviceCache.requestCamera(tile.rc, 1, _mp);

    cuda_volumeRetrieveBestDepth(_depthSimMap_dmp,   // output depth/sim map
                                 _depths_dmp,        // rc depth
                                 _volumeBestSim_dmp, // second best sim volume optimized in best sim volume
                                 rcDeviceCamera,
                                 _sgmParams,
                                 depthRange,
                                 downscaledRoi, 
                                 _stream);

    ALICEVISION_LOG_INFO(tile << "SGM Retrieve best depth in volume done.");
}

void Sgm::exportVolumeInformation(const Tile& tile,
                                  const SgmDepthList& tileDepthList,
                                  const CudaDeviceMemoryPitched<TSim, 3>& in_volume_dmp,
                                  const std::string& name) const
{
    if(!_sgmParams.exportIntermediateVolumes && 
       !_sgmParams.exportIntermediateCrossVolumes &&
       !_sgmParams.exportIntermediateVolume9pCsv)
    {
        // nothing to do
        return;
    }

    // get file tile begin indexes (default is single tile)
    int tileBeginX = -1;
    int tileBeginY = -1;

    if(tile.nbTiles > 1)
    {
        tileBeginX = tile.roi.x.begin;
        tileBeginY = tile.roi.y.begin;
    }

    // copy device similarity volume to host memory
    CudaHostMemoryHeap<TSim, 3> volumeSim_hmh(in_volume_dmp.getSize());
    volumeSim_hmh.copyFrom(in_volume_dmp);

    if(_sgmParams.exportIntermediateVolumes)
    {
        ALICEVISION_LOG_INFO(tile << "Export similarity volume (" << name << ").");

        const std::string volumePath = getFileNameFromIndex(_mp, tile.rc, mvsUtils::EFileType::volume, _sgmParams.scale, "_" + name, tileBeginX, tileBeginY);
        
        exportSimilarityVolume(volumeSim_hmh, tileDepthList.getDepths(), _mp, tile.rc, _sgmParams, volumePath, tile.roi);

        ALICEVISION_LOG_INFO(tile << "Export similarity volume (" << name << ") done.");
    }


    if(_sgmParams.exportIntermediateCrossVolumes)
    {
        ALICEVISION_LOG_INFO(tile << "Export similarity volume cross (" << name << ").");

        const std::string volumeCrossPath = getFileNameFromIndex(_mp, tile.rc, mvsUtils::EFileType::volumeCross, _sgmParams.scale, "_" + name, tileBeginX, tileBeginY);

        exportSimilarityVolumeCross(volumeSim_hmh, tileDepthList.getDepths(), _mp, tile.rc, _sgmParams, volumeCrossPath, tile.roi);

        ALICEVISION_LOG_INFO(tile << "Export similarity volume cross (" << name << ") done.");
    }


    if(_sgmParams.exportIntermediateVolume9pCsv)
    {
        ALICEVISION_LOG_INFO(tile << "Export similarity volume 9 points CSV (" << name << ").");

        const std::string stats9Path = getFileNameFromIndex(_mp, tile.rc, mvsUtils::EFileType::stats9p, _sgmParams.scale, "_sgm", tileBeginX, tileBeginY);

        exportSimilaritySamplesCSV(volumeSim_hmh, tileDepthList.getDepths(), tile.rc, name, stats9Path);

        ALICEVISION_LOG_INFO(tile << "Export similarity volume 9 points CSV (" << name << ") done.");
    }
}

} // namespace depthMap
} // namespace aliceVision