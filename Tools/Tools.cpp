// Tools.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include "GameWindows.h"
#include "GameTime.h"

int main()
{
	GameWindows gameWindow/*(800, 600, "Game Window")*/;
    gameWindow.OnInitialize();
	gameWindow.SetWindowTitle(L"Game Window");
	gameWindow.SetWindowSize(800, 600);


    GameTime::Config config;
    config.fixedDeltaTime = 1.0 / 60.0;
    config.maxDeltaTime = 0.25;
    config.maxPhysicsStepsPerFrame = 8;
    config.timeScale = 1.0;
    config.clearPhysicsAccumulatorOnReset = true;

    GameTime gameTime;
    gameTime.Reset();
    gameTime.SetConfig(config);

    GameInput::AddKeyCallback([](GameInput::KeyCode key, GameInput::KeyState state)
        {
            if (key == GameInput::KeyCode::A && state == GameInput::KeyState::Pressed)
            {
                std::cout << "A key pressed." << std::endl;
            }
        });

    while (gameWindow.IsRunning())
    {
		GameInput::Update();
        gameTime.OnUpdate();

        while (gameTime.UpdatePhysics())
        {
   //         const double fixedDt = gameTime.FixedDeltaTime();
			//std::cout << "Physics update with fixed delta time: " << fixedDt << " seconds." << std::endl;
        }


        gameWindow.PumpMessages();
    }
	return 0;
}


