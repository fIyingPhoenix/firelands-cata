/*
 * This file is part of the FirelandsCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _MODULE_MGR_H_
#define _MODULE_MGR_H_

#include "Define.h"
#include "Util.h"
#include <string>
#include <string_view>
#include <vector>

namespace Firelands::Module
{
FC_COMMON_API void SetEnableModulesList(std::string_view modulesList);
FC_COMMON_API std::vector<std::string_view> GetEnableModulesList();
} // namespace Firelands::Module

#endif