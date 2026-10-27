#pragma once

#include "weread_types.h"

class WereadAuthStore {
public:
    bool Load(WereadAuth* auth) const;
    bool Save(const WereadAuth& auth) const;
    bool Clear() const;
};
