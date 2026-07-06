#include "pandabase.h"
#include "pointerToArray.h"
#include "pvector.h"
#include <string>

using namespace std;

void test_helpers() {
    pvector<string> v_str;
    v_str.push_back("hello");
    const char *s = v_str[0].c_str();

    PointerToArray<unsigned char> pta_uc;
    pta_uc.push_back((unsigned char)5);
    unsigned char uc = pta_uc[0];
}
