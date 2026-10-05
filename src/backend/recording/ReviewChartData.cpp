#include "backend/recording/ReviewChartData.h"
#include "ReviewIsoelasticData.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace backend::recording {
ReviewChartData makeReviewChartData(const std::vector<services::ProcessedFrame>& frames,double factor,double ringMin,double ringMax) {
    if(!std::isfinite(factor) || factor<=0 || !std::isfinite(ringMin) || !std::isfinite(ringMax) || ringMax<=ringMin || ringMax-ringMin>512)
        throw std::invalid_argument("Invalid chart calibration or histogram range");
    ReviewChartData data;data.ringMin=ringMin;data.ringMax=ringMax;
    data.bins.resize(std::max(1,static_cast<int>((ringMax-ringMin)/0.5)));
    for(const auto& frame:frames) {
        if(!frame.validation.isValid)continue;
        const double area=frame.validation.area*factor*factor,deform=frame.validation.deformability;
        if(std::isfinite(area) && std::isfinite(deform))data.points.emplace_back(area,deform);
        else ++data.excludedNonfinite;
        const double ratio=frame.validation.ringRatio;
        if(std::isfinite(ratio) && ratio>0) {
            data.ringRatios.push_back(ratio);
            const auto bin=std::min(data.bins.size()-1,static_cast<size_t>((std::clamp(ratio,ringMin,ringMax)-ringMin)/0.5));
            ++data.bins[bin];
        }
    }
    if(!data.points.empty()) {
        double aMin=std::numeric_limits<double>::max(),aMax=std::numeric_limits<double>::lowest(),dMin=aMin,dMax=aMax;
        for(const auto& [area,deform]:data.points){aMin=std::min(aMin,area);aMax=std::max(aMax,area);dMin=std::min(dMin,deform);dMax=std::max(dMax,deform);}
        const auto pad=[](double low,double high,bool upper) {const long double margin=(static_cast<long double>(high)-low)*0.1L;const long double value=upper?static_cast<long double>(high)+margin:static_cast<long double>(low)-margin;return static_cast<double>(std::clamp(value,-static_cast<long double>(std::numeric_limits<double>::max()),static_cast<long double>(std::numeric_limits<double>::max())));};
        if(aMin<aMax){data.areaMin=pad(aMin,aMax,false);data.areaMax=pad(aMin,aMax,true);}
        else {data.areaMin=std::min(0.0,aMin-0.5);data.areaMax=std::max(1000.0,aMax+0.5);}
        if(dMin<dMax){data.deformMin=pad(dMin,dMax,false);data.deformMax=pad(dMin,dMax,true);}
        else {data.deformMin=std::min(0.0,dMin-0.05);data.deformMax=std::max(1.0,dMax+0.05);}
    }
    return data;
}
const IsoelasticCurves& bundledIsoelasticCurves() {
    static const auto curves=[] {
        IsoelasticCurves result;std::istringstream input(reviewIsoelasticData());std::string line;
        while(std::getline(input,line)) {
            if(line.empty() || line.front()=='#')continue;
            double area,deform,modulus;std::istringstream fields(line);
            if(fields>>area>>deform>>modulus && std::isfinite(area) && std::isfinite(deform) && std::isfinite(modulus))result[modulus].emplace_back(area,deform);
        }
        return result;
    }();
    return curves;
}
std::map<std::string,cv::Mat> renderReviewCharts(const ReviewChartData& data,bool overlays) {
    constexpr int size=1200,left=110,right=1120,top=100,bottom=1070;
    const cv::Scalar black(20,20,20),green(45,140,35);
    cv::Mat scatter(size,size,CV_8UC3,cv::Scalar(255,255,255)),histogram=scatter.clone();
    const auto text=[&](cv::Mat& image,const std::string& value,int x,int y,double scale=0.7){cv::putText(image,value,{x,y},cv::FONT_HERSHEY_SIMPLEX,scale,black,1,cv::LINE_AA);};
    const auto label=[](double value){std::ostringstream out;out.precision(5);out<<value;return out.str();};
    const auto normalized=[](double value,double low,double high){return static_cast<double>(std::clamp((static_cast<long double>(value)-low)/(static_cast<long double>(high)-low),-2.0L,3.0L));};
    const auto point=[&](double area,double deform){return cv::Point(left+static_cast<int>(normalized(area,data.areaMin,data.areaMax)*(right-left)),bottom-static_cast<int>(normalized(deform,data.deformMin,data.deformMax)*(bottom-top)));};
    cv::line(scatter,{left,top},{left,bottom},black,2);cv::line(scatter,{left,bottom},{right,bottom},black,2);
    text(scatter,"Deformability vs Area (um^2) - all valid objects",left,50);text(scatter,"Area (um^2)",500,1160);
    text(scatter,label(data.areaMin),left,1110);text(scatter,label(data.areaMax),right-80,1110);text(scatter,label(data.deformMin),10,bottom);text(scatter,label(data.deformMax),10,top);
    if(overlays) {
        int colorIndex=0;
        for(const auto& [modulus,curve]:bundledIsoelasticCurves()) {
            const cv::Scalar color(70+(colorIndex*71)%150,50+(colorIndex*41)%150,70+(colorIndex*53)%150);++colorIndex;
            const int legendY=top+colorIndex*22;
            cv::line(scatter,{right-145,legendY},{right-115,legendY},color,2);
            text(scatter,label(modulus)+" kPa",right-108,legendY+5,0.45);
            for(size_t i=1;i<curve.size();++i) {
                auto a=point(curve[i-1].first,curve[i-1].second),b=point(curve[i].first,curve[i].second);
                if(cv::clipLine(cv::Rect(left,top,right-left,bottom-top),a,b))cv::line(scatter,a,b,color,1,cv::LINE_AA);
            }
        }
        text(scatter,"Isoelastic curves: bundled 30 um / 0.25 flow / 4.24 viscosity",left,80,0.55);
    }
    for(const auto& [area,deform]:data.points)cv::circle(scatter,point(area,deform),2,green,-1,cv::LINE_AA);
    cv::line(histogram,{left,top},{left,bottom},black,2);cv::line(histogram,{left,bottom},{right,bottom},black,2);
    text(histogram,"Ring-ratio distribution - all valid objects",left,50);text(histogram,"Ring ratio (dimensionless)",400,1160);
    const auto maxBin=std::max<uint64_t>(1,*std::max_element(data.bins.begin(),data.bins.end()));
    for(size_t i=0;i<data.bins.size();++i){const int x1=left+i*(right-left)/data.bins.size(),x2=left+(i+1)*(right-left)/data.bins.size();const int y=bottom-static_cast<int>(static_cast<double>(data.bins[i])/maxBin*(bottom-top));cv::rectangle(histogram,{x1,y},{std::max(x1,x2-1),bottom},green,-1);}
    text(histogram,label(data.ringMin),left,1110);text(histogram,label(data.ringMax),right-80,1110);text(histogram,std::to_string(maxBin),10,top);
    return {{"scatter_plot.tiff",scatter},{"histogram.tiff",histogram}};
}
} // namespace backend::recording
