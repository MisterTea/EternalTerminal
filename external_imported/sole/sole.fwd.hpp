#pragma once
#include <sys/types.h>
#include <stdint.h>
#include <string>
namespace sole {
    struct uuid;
    uuid uuid0();
    uuid uuid1();
    uuid uuid4();
    uuid rebuild( uint64_t ab, uint64_t cd );
    uuid rebuild( const std::string &uustr );
}