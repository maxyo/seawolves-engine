#pragma once

// The script VM's differential checks (-DSTORM_VM_VERIFY=ON, never shipped builds; tools/vm_verify/README.md).
// Every fast path of the VM that replaced a plain one (a lookup through an index, a cached conversion, a formatter
// of our own) also computes the plain answer in such a build and stops the engine at the first difference, so a
// session of the real game checks each of them millions of times. The counts are logged when the engine exits.
#ifdef STORM_VM_VERIFY
#include <cstdint>

namespace storm::vm_verify
{
enum class Check : uint8_t
{
    NameCode,        // STRING_CODEC: the index against a walk of the name's bucket
    AttributeByCode, // ATTRIBUTES: the dense code scan against the children's own codes
    AttributeByIndex, // ATTRIBUTES: the hash index of an object with many attributes against the same
    AttributeByName, // ATTRIBUTES: the code of the name against comparing the children's names
    FloatText,       // a float written to an attribute: our "%.7f" against printf's
    EventStatuses,   // S_EVENTTAB: a frame skipped as having no new or deleted handler against a walk of the table
    Count
};

void Count(Check check) noexcept;
// Logs the difference and aborts.
[[noreturn]] void Mismatch(const char *format, ...) noexcept __attribute__((format(printf, 1, 2)));
// One line with the number of checks of every kind (the session's evidence); also written at exit.
void Report() noexcept;
} // namespace storm::vm_verify
#endif
