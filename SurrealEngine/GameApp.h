#pragma once

class GameApp
{
public:
	int main(Array<std::string> args);

private:
#ifdef __EMSCRIPTEN__
	static void BrowserFrame();
#endif
};
