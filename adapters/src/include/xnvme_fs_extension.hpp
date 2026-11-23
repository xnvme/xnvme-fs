// SPDX-FileCopyrightText: Simon A. F. Lund <os@safl.dk>
// SPDX-FileCopyrightText: Stichting DuckDB Foundation
//
// SPDX-License-Identifier: MIT

#pragma once

#include "duckdb.hpp"

namespace duckdb {

class XnvmeFsExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
