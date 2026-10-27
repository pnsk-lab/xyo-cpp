#pragma once

namespace xyo {
namespace v8math {

// V8 の Math.* と同じ結果を返す（fdlibm 準拠）。
double acos(double x);
double asin(double x);
double atan(double x);
double atan2(double y, double x);
double sin(double x);
double cos(double x);
double exp(double x);

}  // namespace v8math
}  // namespace xyo
