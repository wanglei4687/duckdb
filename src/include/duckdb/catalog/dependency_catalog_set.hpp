#pragma once

#include "duckdb/catalog/catalog_set.hpp"

namespace duckdb {

//! This class mocks the CatalogSet interface, but does not actually store CatalogEntries
class DependencyCatalogSet {
public:
	DependencyCatalogSet(CatalogSet &set, const MangledEntryName &mangled_name, CatalogEntryInfo info)
	    : set(set), mangled_name(mangled_name), info(info) {
	}

public:
	bool CreateEntry(CatalogTransaction transaction, const MangledEntryName &name, unique_ptr<CatalogEntry> value);
	CatalogSet::EntryLookup GetEntryDetailed(CatalogTransaction transaction, const MangledEntryName &name);
	optional_ptr<CatalogEntry> GetEntry(CatalogTransaction transaction, const MangledEntryName &name);
	void Scan(CatalogTransaction transaction, const std::function<void(CatalogEntry &)> &callback);
	bool DropEntry(CatalogTransaction transaction, const MangledEntryName &name, bool cascade,
	               bool allow_drop_internal = false);

private:
	MangledDependencyName ApplyPrefix(const MangledEntryName &name) const;

public:
	CatalogSet &set;
	MangledEntryName mangled_name;

	// TODO: remove these later
	CatalogEntryInfo info;
};

} // namespace duckdb
