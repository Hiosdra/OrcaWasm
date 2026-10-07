#ifndef BOOST_NUMERIC_INTERVAL_DETAIL_EMSCRIPTEN_ROUNDING_UNAVAILABLE_HPP
#define BOOST_NUMERIC_INTERVAL_DETAIL_EMSCRIPTEN_ROUNDING_UNAVAILABLE_HPP

namespace boost {
namespace numeric {
namespace interval_lib {
namespace detail {

// WebAssembly has no directed floating-point rounding mode. Keep CGAL's
// umbrella headers parseable, but make any instantiated Boost.Interval
// arithmetic fail at compile time instead of silently returning unsafe bounds.
template<class T>
struct emscripten_rounding_unavailable
{
    typedef int rounding_mode;

    static void get_rounding_mode(rounding_mode &) = delete;
    static void set_rounding_mode(rounding_mode) = delete;
    static void upward() = delete;
    static void downward() = delete;
    static void to_nearest() = delete;
    static T to_int(const T &) = delete;
    static T force_rounding(const T &) = delete;
};

} // namespace detail
} // namespace interval_lib
} // namespace numeric
} // namespace boost

#endif // BOOST_NUMERIC_INTERVAL_DETAIL_EMSCRIPTEN_ROUNDING_UNAVAILABLE_HPP
