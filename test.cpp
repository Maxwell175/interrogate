#include <iostream>
#include <string>

int main() {
    std::string type_name = "IConstPointerToArray_LVecBase2d";
    std::string prefix = "IConstPointerToArray_";
    if (type_name.compare(0, prefix.size(), prefix) == 0) {
        std::cout << type_name.substr(prefix.size()) << std::endl;
    }
    return 0;
}
