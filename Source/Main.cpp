#include <juce_gui_basics/juce_gui_basics.h>
#include "MainComponent.h"

class PatchBayApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "PatchBay"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String&) override
    {
        lookAndFeel.setColourScheme (juce::LookAndFeel_V4::getMidnightColourScheme());
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }
    void shutdown() override
    {
        mainWindow.reset();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }
    void systemRequestedQuit() override            { quit(); }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name)
            : DocumentWindow (name,
                              juce::Desktop::getInstance().getDefaultLookAndFeel()
                                  .findColour (ResizableWindow::backgroundColourId),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new pb::MainComponent(), true);
           #if JUCE_IOS || JUCE_ANDROID
            setFullScreen (true);
           #else
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
           #endif
            setVisible (true);
        }

        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

private:
    juce::LookAndFeel_V4 lookAndFeel;     // declared before the window: outlives every component using it
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (PatchBayApplication)
