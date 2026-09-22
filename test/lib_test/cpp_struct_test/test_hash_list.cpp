#include "cpp_struct/hash_list.hpp"

#include <cassert>
#include <cstdlib>
#include <utility>

int
main()
{
    HashList l{};

    l.access(0);
    l.access(1);
    l.access(2);
    l.access(0);
    std::free(l.extract_head());
    std::free(l.extract_head());
    std::free(l.extract_head());
    std::free(l.extract_head());
    assert(l.size() == 0);

    // Removing an absent key must be a harmless no-op.
    assert(!l.remove(1234));
    assert(l.size() == 0);

    l.access(10);
    l.access(20);
    HashList moved{std::move(l)};
    assert(l.size() == 0);
    assert(!l.front().has_value());
    assert(moved.size() == 2);
    assert(moved.front() == 10);
    assert(moved.back() == 20);

    // A moved-from list remains usable and owns only its new nodes.
    l.access(30);
    assert(l.contains(30));
    assert(!moved.contains(30));

    return 0;
}
