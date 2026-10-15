#pragma once

#include <string>

class WebGLFileResource
{
public:
	static std::string readAllText(const std::string& filename);
};
