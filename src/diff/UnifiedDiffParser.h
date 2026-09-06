#pragma once
#include "DiffModel.h"
#include <string_view>
namespace gdv
{
std::wstring fromUtf8(std::string_view text);
std::string toUtf8(const std::wstring &text);
class UnifiedDiffParser
{
public:
  DiffDocument parse(std::string_view patch) const;
};
} // namespace gdv
