# Pinned Boost.Asio build recipe

Copied from microsoft/vcpkg at `04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4`, directory `ports/boost-asio` (Boost1.92.0). The upstream source archive SHA-512 and patch are unchanged. The only recipe policy change is an empty default-feature set (local port revision1).

Devbox uses C++23 coroutine support and steady timers. It does not use legacy deadline_timer, stackful spawn, or Boost.Regex overloads. Empty defaults prevent transitive callers from re-enabling those optional feature dependencies. BOOST_ASIO_DISABLE_BOOST_REGEX is also explicit in the C++ build. This removes the unused date-time -> algorithm/range -> regex dependency path; it is not a patch or suppression of a vulnerability in a shipped regex implementation.

Upstream recipe source: https://github.com/microsoft/vcpkg/tree/04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4/ports/boost-asio
Boost.Asio feature documentation: https://www.boost.org/doc/libs/latest/doc/html/boost_asio/using.html
The build recipe is under the accompanying vcpkg MIT license. Boost library notices remain in the resolved release inventory.
