#pragma once
#include "Framework/L2DBaseModel.hpp"
#include "Framework/L2DTargetPoint.hpp"
#include "Framework/MatrixManager.hpp"
#include "Model/Live2DModelOpenGL.hpp"
#include <functional>
#include <string>


namespace live2d {
class LAppModel : public L2DBaseModel {
public:
    using StartCallback = std::function<void(const std::string&, int)>;
    using FinishCallback = std::function<void(const std::string&, int)>;

    LAppModel();
    ~LAppModel() override;
    bool loadModelJson(const std::string& path);
    void resize(int w, int h);
    void drag(float x, float y);
    void touch(float x, float y);
    bool isMotionFinished() const;
    void setOffset(float dx, float dy);
    void setScale(float s);
    void setParameterValue(const std::string& id, float val, float weight = 1.0f);
    void addParameterValue(const std::string& id, float val, float weight = 1.0f);
    void setAutoBreathEnable(bool v) { mAutoBreath = v; }
    void setAutoBlinkEnable(bool v) { mAutoBlink = v; }
    int getParameterCount() const;
    int getPartCount() const;
    std::string getPartId(int index) const;
    void setPartOpacity(int index, float val);
    // deltaSec is the wall-clock time since the previous update. It only feeds
    // the drag/gaze easing, which is an exponential approach and so needs a real
    // interval to converge at a rate rather than at a frame count. The default
    // keeps upstream's call sites compiling; hosts that know their frame time
    // should pass it.
    void update(float deltaSec = 1.0f / 60.0f);
    void draw();
    bool hitTest(const std::string& area, float x, float y);
    void setExpression(const std::string& name);
    void setRandomExpression();
    void startMotion(const std::string& group, int no, int priority,
                     StartCallback onStart = nullptr, FinishCallback onFinish = nullptr);
    void startRandomMotion(const std::string& group, int priority,
                           StartCallback onStart = nullptr, FinishCallback onFinish = nullptr);
    void clearMotions();
    void stopAllMotions() { clearMotions(); }
    void resetExpression();
    void resetPose();
    float getCanvasWidth() const { return (float)mLive2DModel->getCanvasWidth(); }
    float getCanvasHeight() const { return (float)mLive2DModel->getCanvasHeight(); }
    int getPixelsPerUnit() const { return 1; }
    void rotate(float deg);
    float getParameterValue(int index) const;
    float getParameterMin(int index) const;
    float getParameterMax(int index) const;
    float getParameterDefault(int index) const;
    std::string getParameterId(int index) const;
    void setPartScreenColor(int index, float r, float g, float b, float a);
    void setPartMultiplyColor(int index, float r, float g, float b, float a);
    std::vector<float> getPartScreenColor(int index) const;
    std::vector<float> getPartMultiplyColor(int index) const;
    std::vector<std::string> hitPart(float x, float y, bool topOnly);
    // LOCAL ADDITION (see ../PATCHES.md). Bounds are in model canvas
    // coordinates: origin top-left, +y downward, extent getCanvasWidth() x
    // getCanvasHeight(). Both write {left, top, right, bottom} and return false
    // if nothing measurable was found.
    //
    // getDrawableBounds() measures one named drawable, which is how the
    // D_REF.PT_* hit-area rectangles are read back for automatic framing.
    // getFigureBounds() measures the union of every visible drawable, i.e. the
    // silhouette of the character herself, ignoring the invisible D_REF markers.
    //
    // refreshGeometry() runs the deformer chain without drawing; draw() normally
    // does this, so bounds read before the first frame are otherwise all zero.
    void refreshGeometry();
    bool getDrawableBounds(const std::string& drawId, float out[4]) const;
    bool getFigureBounds(float out[4]) const;
    void setTexture(int no, int texId);
    L2DTargetPoint mDragMgr;
    MatrixManager mMatrixManager;
    bool mAutoBreath = true, mAutoBlink = true;
    bool mClearFlag = false;
    std::string mModelHomeDir;
private:
    StartCallback mOnStartMotion;
    FinishCallback mOnFinishMotion;
    bool mCallbacksPending = false;
    std::string mCurrentGroup; int mCurrentNo = 0;
};
} // namespace live2d
