#ifndef IPV6_ADDRESS_COMPAT_H
#define IPV6_ADDRESS_COMPAT_H

#include <stdint.h>
#include <string.h>

class IPv6Address {
public:
    IPv6Address() {
        memset(_address, 0, sizeof(_address));
    }

    explicit IPv6Address(const uint32_t address[4]) {
        memcpy(_address, address, sizeof(_address));
    }

    explicit IPv6Address(const uint8_t address[16]) {
        memcpy(_address, address, sizeof(_address));
    }

    operator const uint32_t *() const {
        return _address;
    }

    operator uint32_t *() {
        return _address;
    }

private:
    uint32_t _address[4];
};

#endif
