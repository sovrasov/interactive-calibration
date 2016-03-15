#include <opencv2/core.hpp>
#include <opencv2/calib3d.hpp>
#include <opencv2/aruco/charuco.hpp>
#include <string>
#include <vector>
#include <stack>
#include <exception>
#include <algorithm>
#include <iostream>
#include <ctime>
#include <chrono>

#include "calibCommon.hpp"
#include "calibPipeline.hpp"
#include "frameProcessor.hpp"
#include "cvCalibrationFork.hpp"
#include "calibController.hpp"

using namespace calib;

const char* keys  =
        "{n        | 20      | Number of frames for calibration }"
        "{v        |         | Input from video file }"
        "{ci       | 0       | DefaultCameraID }"
        "{si       | false   | Save captured frames }"
        "{flip     | false   | Vertical flip of input frames }"
        "{t        | circles | Template for calibration (circles, chessboard, dualCircles, chAruco) }"
        "{sz       | 163     | Distance between two nearest centers of circles or squares on calibration board}"
        "{dst      | 295     | Distance between white and black parts of daulCircles template}"
        "{w        |         | Width of template (in corners or circles)}"
        "{h        |         | Height of template (in corners or circles)}"
        "{of       | CamParams.xml | Output file name}"
        "{ft       | true    | Auto tuning of calibration flags}";

void saveCalibrationParameters(Sptr<calibrationData> data, const std::string& fileName)
{
    cv::FileStorage parametersWriter(fileName, cv::FileStorage::WRITE);
    time_t rawtime;
    time(&rawtime);
    parametersWriter << "calibrationDate" << asctime(localtime(&rawtime));
    parametersWriter << "framesCount" << std::max((int)data->objectPoints.size(), (int)data->allCharucoCorners.size());
    parametersWriter << "cameraMatrix" << data->cameraMatrix;
    parametersWriter << "cameraMatrix_std_dev" << data->stdDeviations.rowRange(cv::Range(0, 4));
    parametersWriter << "dist_coeffs" << data->distCoeffs;
    parametersWriter << "dist_coeffs_std_dev" << data->stdDeviations.rowRange(cv::Range(4, 9));
    parametersWriter << "avg_reprojection_error" << data->totalAvgErr;

    parametersWriter.release();
}

void deleteButton(int state, void* data)
{
    Sptr<calibDataController> controller = *((Sptr<calibDataController>*)data);
    controller->deleteLastFrame();
}

void deleteAllButton(int state, void* data)
{
    Sptr<calibDataController> controller = *((Sptr<calibDataController>*)data);
    controller->deleteAllData();
}

int main(int argc, char** argv)
{
    cv::CommandLineParser parser(argc, argv, keys);

    captureParameters capParams;

    capParams.flipVertical = parser.get<bool>("flip");

    if (parser.has("v")) {
        capParams.source = InputVideoSource::File;
        capParams.videoFileName = parser.get<std::string>("v");
    }
    else {
        capParams.source = InputVideoSource::Camera;
        capParams.camID = parser.get<int>("ci");
    }

    auto templateType = parser.get<std::string>("t");

    if(templateType.find("circles", 0) == 0) {
        capParams.board = TemplateType::AcirclesGrid;
        capParams.boardSize = cv::Size(4, 11);
    }
    else if(templateType.find("chessboard", 0) == 0) {
        capParams.board = TemplateType::Chessboard;
        capParams.boardSize = cv::Size(7, 7);
    }
    else if(templateType.find("dualcircles", 0) == 0) {
        capParams.board = TemplateType::DoubleAcirclesGrid;
        capParams.boardSize = cv::Size(4, 11);
    }
    else if(templateType.find("charuco", 0) == 0)
        capParams.board = TemplateType::chAruco;

    if(parser.has("w") && parser.has("h")) {
        capParams.boardSize = cv::Size(parser.get<int>("w"), parser.get<int>("h"));
        if (capParams.boardSize.width <= 0 || capParams.boardSize.height <= 0) {
            std::cerr << "Board size must be positive\n";
            return 0;
        }
    }
    if(parser.get<std::string>("of").find(".xml") <= 0) {
        std::cerr << "Wrong output file name: correct format is [name].xml\n";
        return 0;
    }

    Sptr<calibrationData> globalData(new calibrationData);

    int calibrationFlags = 0;
    Sptr<calibController> controller(new calibController(globalData, calibrationFlags, parser.get<bool>("ft")));
    Sptr<calibDataController> dataController(new calibDataController(globalData));

    Sptr<FrameProcessor> capProcessor, showProcessor;
    capProcessor = Sptr<FrameProcessor>(new CalibProcessor(globalData, capParams.board, capParams.boardSize));
    showProcessor = Sptr<FrameProcessor>(new ShowProcessor(globalData, controller));

    Sptr<CalibPipeline> pipeline(new CalibPipeline(capParams, controller));
    std::vector<Sptr<FrameProcessor>> processors;
    processors.push_back(capProcessor);
    processors.push_back(showProcessor);

    cv::namedWindow(mainWindowName);
    cv::moveWindow(mainWindowName, 0, 0);
    cv::createButton("Delete last frame", deleteButton, &dataController, cv::QT_PUSH_BUTTON, false);
    cv::createButton("Delete all frames", deleteAllButton, &dataController, cv::QT_PUSH_BUTTON, false);

    try {
        while(true)
        {
            auto exitStatus = pipeline->start(processors);
            if (exitStatus == PipelineExitStatus::Finished) {
                //std::cout << "Calibration finished\n";
                if(controller->getCommonCalibrationState())
                    saveCalibrationParameters(globalData, parser.get<std::string>("of"));
                break;
            }
            else if (exitStatus == PipelineExitStatus::Calibrate) {

                dataController->rememberCurrentParameters();
                globalData->imageSize = pipeline->getImageSize();
                calibrationFlags = controller->getNewFlags();

                using namespace std::chrono;
                auto startPoint = high_resolution_clock::now();
                if(capParams.board != TemplateType::chAruco)
                {
                    globalData->totalAvgErr = cvfork::calibrateCamera(globalData->objectPoints, globalData->imagePoints, globalData->imageSize, globalData->cameraMatrix,
                                    globalData->distCoeffs, cv::noArray(), cv::noArray(), globalData->stdDeviations, calibrationFlags, cv::TermCriteria(
                                        cv::TermCriteria::COUNT+cv::TermCriteria::EPS, 30, 1e-7) );
                }
                else {
                    cv::Ptr<cv::aruco::Dictionary> dictionary =
                            cv::aruco::getPredefinedDictionary(cv::aruco::PREDEFINED_DICTIONARY_NAME(0));
                    cv::Ptr<cv::aruco::CharucoBoard> charucoboard =
                                cv::aruco::CharucoBoard::create(6, 8, 200, 100, dictionary);
                    globalData->totalAvgErr =
                            cvfork::calibrateCameraCharuco(globalData->allCharucoCorners, globalData->allCharucoIds, charucoboard, globalData->imageSize,
                                                          globalData->cameraMatrix, globalData->distCoeffs, cv::noArray(), cv::noArray(), globalData->stdDeviations, calibrationFlags,
                                                           cv::TermCriteria(cv::TermCriteria::COUNT+cv::TermCriteria::EPS, 30, 1e-7));
                }
                auto endPoint = high_resolution_clock::now();
                //std::cout << "Calibration time: " << (duration_cast<duration<double>>(endPoint - startPoint)).count() << "\n";
            }
            else if (exitStatus == PipelineExitStatus::DeleteLastFrame)
                dataController->deleteLastFrame();
            else if (exitStatus == PipelineExitStatus::DeleteAllFrames)
                dataController->deleteAllData();
            else if (exitStatus == PipelineExitStatus::SaveCurrentData)
                saveCalibrationParameters(globalData, parser.get<std::string>("of"));
            else if (exitStatus == PipelineExitStatus::SwitchUndistort)
                static_cast<ShowProcessor*>(showProcessor.get())->switchUndistort();

            for (auto it = processors.begin(); it != processors.end(); ++it)
                        (*it)->resetState();
        }
    }
    catch (std::runtime_error exp)
    {
        std::cout << exp.what() << std::endl;
    }

    return 0;
}
