
#include "Precomp.h"
#include "Utils/Exception.h"
#include "Utils/Logger.h"
#include "Utils/CommandLine.h"
#include "GameApp.h"
#include "GameFolder.h"
#include "Engine.h"
#include "UI/WidgetResourceData.h"
#include "UI/ErrorWindow/ErrorWindow.h"
#include "UI/Launcher/LauncherWindow.h"
#include "Utils/File.h"
#include <stdexcept>
#include <iostream>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef __EMSCRIPTEN__
// The browser owns the frame loop: a blocking while loop never returns control to it, so
// the canvas is never presented and the page hangs. Instead Engine::TickGame runs once per
// animation frame from emscripten_set_main_loop, and main() unwinds (simulate_infinite_loop)
// while the game keeps running. Anything the running game needs must therefore outlive
// GameApp::main's stack frame, which is why these live here.
namespace
{
	std::unique_ptr<CommandLine> browserCommandLine;
	std::unique_ptr<Engine> browserEngine;
}

void GameApp::BrowserFrame()
{
	try
	{
		if (browserEngine && browserEngine->TickGame())
			return;

		emscripten_cancel_main_loop();
		if (browserEngine)
		{
			browserEngine->Shutdown();
			browserEngine.reset();
		}
		DeinitWidgetResources();
		emscripten_force_exit(0);
	}
	catch (const std::exception& e)
	{
		// The error window runs a nested event loop, which cannot be entered from inside an
		// animation frame callback, so the message goes to the log/console instead.
		LogMessage(std::string("Unhandled exception in game loop: ") + e.what());
		std::cout << e.what() << std::endl;
		emscripten_cancel_main_loop();
		emscripten_force_exit(1);
	}
}
#endif

int GameApp::main(Array<std::string> args)
{
	InitWidgetResources("dark");

	try
	{
#ifdef __EMSCRIPTEN__
		browserCommandLine = std::make_unique<CommandLine>(args);
		commandline = browserCommandLine.get();
#else
		CommandLine cmd(args);
		commandline = &cmd;
#endif

		if (ErrorWindow::CheckCrashReporter())
			return 0;

		if (commandline->HasArg("-h", "--help"))
		{
			std::cout << "SurrealEngine [--url=<mapname>] [--engineversion=X] [Path to game folder]\n";
			return 0;
		}

		int selectedGameIndex = LauncherWindow::ExecModal();
		if (selectedGameIndex >= 0)
		{
			GameLaunchInfo info = GameFolderSelection::GetLaunchInfo(selectedGameIndex);
#ifdef __EMSCRIPTEN__
			browserEngine = std::make_unique<Engine>(info);
			browserEngine->Startup();
			// Does not return: unwinds out of main() and keeps the runtime alive for BrowserFrame.
			emscripten_set_main_loop(&GameApp::BrowserFrame, 0, true);
#else
			Engine engine(info);
			engine.Run();
#endif
		}
	}
	catch (const std::exception& e)
	{
		ErrorWindow::ExecModal(e.what(), Logger::Get()->GetLog());
	}

	DeinitWidgetResources();
	return 0;
}
