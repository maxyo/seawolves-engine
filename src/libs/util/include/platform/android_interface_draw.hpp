#pragma once

namespace storm::android
{
// Suppress only GPU submissions made inside XINTERFACE::Realize. Its nodes
// must still run Draw: several update animation, selection and script state.
// The scope never covers the world renderer or the scene postprocess.
inline thread_local bool nativeInterfaceDrawSuppressed = false;
inline bool NativeInterfaceDrawSuppressed() { return nativeInterfaceDrawSuppressed; }

class NativeInterfaceDrawScope
{
public:
    explicit NativeInterfaceDrawScope(bool suppress) : previous_(nativeInterfaceDrawSuppressed)
    {
        nativeInterfaceDrawSuppressed = previous_ || suppress;
    }
    ~NativeInterfaceDrawScope() { nativeInterfaceDrawSuppressed = previous_; }
    NativeInterfaceDrawScope(const NativeInterfaceDrawScope &) = delete;
    NativeInterfaceDrawScope &operator=(const NativeInterfaceDrawScope &) = delete;

private:
    bool previous_;
};
}
