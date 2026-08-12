// Tools.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include "GameWindows.h"

int main()
{
	GameWindows gameWindow/*(800, 600, "Game Window")*/;

    gameWindow.OnInitialize();
	gameWindow.SetWindowTitle(L"Game Window");
	gameWindow.SetWindowSize(800, 600);
    while (gameWindow.IsRunning())
    {
        //gameTime.OnUpdate();
        //while (gameTime.UpdatePhysics())
        //{
        //    //const double fixedDt = gameTime.FixedDeltaTime();
        //}

        gameWindow.PumpMessages();
    }
	return 0;
}


