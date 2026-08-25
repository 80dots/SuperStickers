#pragma once

namespace autostart {

// HKCU\...\Run 등록 여부
bool IsEnabled();
// 등록/해제. 등록 시 "<exe>" --hidden 으로 기동.
bool SetEnabled(bool enable);

}  // namespace autostart
