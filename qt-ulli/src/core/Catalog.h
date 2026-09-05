// core/Catalog.h
//
// Loads the distro catalog from distros.json. Lookup order:
//   1. <exe-dir>/distros.json               (release layout)
//   2. <exe-parent>/distros.json            (dev layout)
//   3. Built-in FALLBACK_DISTROS            (mirror of distros.json;
//      updated in lockstep with the JSON).
//
// The Catalog is a value-typed, immutable map of Distro objects. The
// loader logs a warning when the file is missing — the UI must surface
// that warning to the user.

#pragma once

#include "core/Distro.h"
#include "core/Result.h"

#include <map>
#include <string>
#include <vector>

namespace ulli::core {

class Catalog {
public:
    Catalog() = default;

    // Try the three locations. `fallbackUsed` is set true if the
    // built-in catalog was used because the file was missing.
    static Catalog load(bool* fallbackUsed = nullptr);

    // Direct construction (used by tests + the fallback).
    static Catalog fromDistros(std::vector<Distro> distros);

    const std::map<std::string, Distro>& distros() const noexcept { return distros_; }

    bool empty() const noexcept { return distros_.empty(); }
    const Distro* find(const std::string& key) const;

    // Internal: used by load() and fromDistros() to build the catalog.
    void insert(Distro d) { distros_.emplace(d.key(), std::move(d)); }

    std::vector<std::string> keys() const;

    // The built-in fallback — kept here so the loader can return it
    // without depending on the JSON file.
    static const std::vector<Distro>& fallbackDistros();

private:
    std::map<std::string, Distro> distros_;
};

}  // namespace ulli::core
