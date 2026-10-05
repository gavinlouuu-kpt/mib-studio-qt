#include "backend/app/AppBackend.h"
#include "backend/app/BackendFacade.h"
#include "backend/recording/Hdf5Service.h"
#include "backend/recording/ReviewChartData.h"
#include "frontend/utils/OverlayRenderer.h"
#include "support/assert.h"
#include "support/tempdir.h"
#include "support/watchdog.h"
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <cstring>
int main() {
    mib::test::Watchdog watchdog(30);
    mib::test::TempDir dir("review_parity");
    const auto path=dir/"fixture.h5";
    backend::services::Hdf5Service writer;
    MIB_REQUIRE(writer.openFile(path.string()) && writer.initializeDatasets(),"create fixture");
    std::vector<backend::services::ProcessedFrame> rows(3);
    for(size_t i=0;i<rows.size();++i) {
        auto& f=rows[i]; f.index=100+i; f.timestampNs=1000+i;
        f.originalImage=cv::Mat(40,40,CV_8UC1,cv::Scalar(60+i));
        f.processedImage=cv::Mat::zeros(40,40,CV_8UC1);
        cv::circle(f.processedImage,{20,20},12,cv::Scalar(255),-1);
        cv::circle(f.processedImage,{20,20},5,cv::Scalar(0),-1);
        f.validation.isValid=true; f.validation.objectCount=1;
        f.validation.area=20+10*i; f.validation.deformability=0.1+0.05*i;
        f.validation.ringRatio=1.0+0.5*i;
    }
    MIB_REQUIRE(writer.appendFrames(rows,{}),"write fixture");writer.closeFile();
#ifdef _WIN32
    _putenv_s("MIB_CAMERA_MODE","mock");
#else
    setenv("MIB_CAMERA_MODE","mock",1);
#endif
    backend::AppBackend backend;backend::bridge::BackendFacade facade(backend);
    MIB_REQUIRE(facade.initialize(dir.path().string()),"initialize");
    backend::bridge::RecordingLoadCommand load;load.filePath=path.string();
    MIB_REQUIRE(facade.dispatch(load).ok,"load");
    backend::services::Hdf5Service reader;MIB_REQUIRE(reader.loadFile(path.string()),"read fixture");
    std::vector<backend::services::ProcessedFrame> saved;MIB_REQUIRE(reader.readValidMetadata(saved),"read metrics");
    const auto cfg=backend.processing().getProcessingConfig();
    const auto expected=backend::recording::makeReviewChartData(saved,backend.processing().getPixelToMicronFactor(),cfg.ring_ratio_min,cfg.ring_ratio_max);
    const auto chart=nlohmann::json::parse(facade.fetchReviewChartsJson());
    MIB_REQUIRE(chart["valid"]==true && chart["finite_points"]=="3","all fixture points");
    MIB_REQUIRE(chart["area_range"][0]==expected.areaMin && chart["area_range"][1]==expected.areaMax,"Qt chart calibration and axes");
    MIB_REQUIRE(chart["deform_range"][0]==expected.deformMin && chart["deform_range"][1]==expected.deformMax,"Qt deform axes");
    for(size_t i=0;i<expected.bins.size();++i)MIB_REQUIRE(chart["histogram"][i]==std::to_string(expected.bins[i]),"Qt histogram exact");
    size_t count=0;for(const auto& cell:chart["density"])count+=std::stoull(cell[2].get<std::string>());
    MIB_REQUIRE(count==expected.points.size(),"density conserves Qt samples");
    MIB_REQUIRE(chart["curves"].size()==backend::recording::bundledIsoelasticCurves().size(),"same bundled curve groups");
    cv::Mat image,mask;MIB_REQUIRE(reader.readImageByIndex("/valid_frames/images",1,image) && reader.readImageByIndex("/valid_frames/masks",1,mask),"saved image and mask");
    for(int mode=0;mode<5;++mode) {
        const auto qt=frontend::createProcessingOverlay(image,mask,&saved[1].validation,static_cast<frontend::OverlayMode>(mode));
        const auto bytes=facade.renderReviewOverlayJson(nlohmann::json{{"source_path",path.string()},{"valid",true},{"index",1},{"mode",mode},{"roi",false}}.dump());
        cv::Mat rgb;cv::cvtColor(cv::imdecode(bytes,cv::IMREAD_COLOR),rgb,cv::COLOR_BGR2RGB);
        MIB_REQUIRE(qt.width()==rgb.cols && qt.height()==rgb.rows,"matching image dimensions");
        for(int y=0;y<rgb.rows;++y)MIB_REQUIRE(std::memcmp(qt.constScanLine(y),rgb.ptr(y),rgb.cols*3)==0,"Qt QImage and Tauri PNG pixel equivalence");
    }
    facade.shutdown();return 0;
}
