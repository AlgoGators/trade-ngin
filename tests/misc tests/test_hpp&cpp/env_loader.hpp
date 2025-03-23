#ifndef ENV_LOADER_HPP
#define ENV_LOADER_HPP

#include <string>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <stdexcept>

class EnvLoader {
public:
    static void load(const std::string& filepath);
};

#endif // ENV_LOADER_HPP
