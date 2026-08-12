#include <iostream>

#include "GameWindows.h"
#include "GameTime.h"
#include "GameLogger.h"

int main()
{
    GameLogger::Config loggerConfig;
    loggerConfig.writeToConsole = true;
    loggerConfig.writeToDebugger = true;
    loggerConfig.writeToFile = true;
    loggerConfig.filePath = "GameTools.log";
    loggerConfig.minimumLevel = GameLogLevel::Trace;

    GameLogger::Initialize(loggerConfig);

    GAME_LOG_INFO("GameTools initialized.");

    GameWindows gameWindow;
    gameWindow.OnInitialize();
    gameWindow.SetWindowTitle(L"Game Window");
    gameWindow.SetWindowSize(800, 600);

    GameTime::Config config;
    config.fixedDeltaTime = 1.0 / 60.0;
    config.maxDeltaTime = 0.25;
    config.maxPhysicsStepsPerFrame = 8;
    config.timeScale = 1.0;
    config.clearPhysicsAccumulatorOnReset = true;

    GameTime gameTime(config);

    GameInput::AddKeyCallback(
        [](GameInput::KeyCode key, GameInput::KeyState state)
        {
            if (key == GameInput::KeyCode::A &&
                state == GameInput::KeyState::Pressed)
            {
                GAME_LOG_DEBUG("A key pressed.");
            }
        });

    while (gameWindow.IsRunning())
    {
        gameWindow.PumpMessages();

        gameTime.OnUpdate();

        while (gameTime.UpdatePhysics())
        {
            const double fixedDt = gameTime.FixedDeltaTime();
            // PhysicsStep(fixedDt);
        }

        // Update(gameTime.DeltaTime());
        // Render();
    }

    GameLogger::Shutdown();

    return 0;
}

