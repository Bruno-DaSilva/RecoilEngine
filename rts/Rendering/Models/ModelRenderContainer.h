/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "Rendering/Models/3DModelDefs.hpp"
#include "System/ContainerUtil.h"
#include "System/ForceInline.hpp"

template<typename T>
RECOIL_FORCE_INLINE const auto& MDL_TYPE(T* o) { return o->model->type; }

template<typename TObject>
class ModelRenderContainerSelector {
public:
	size_t operator()(const TObject* o) const { return size_t(o->model->textureType); }
};

// what the bins store per object (PR 14: drawer containers hold IDs, not
// pointers; draw passes resolve the id through the object's handler)
template<typename TObject>
struct ModelRenderContainerTraits {
	using Handle = int;
	static Handle ToHandle(const TObject* o) { return o->id; }
};

// projectile ids live in two namespaces (synced/unsynced), so their handle
// carries the namespace bit; ToHandle is defined in ProjectileDrawer.h,
// which sees the complete CProjectile type
class CProjectile;
template<>
struct ModelRenderContainerTraits<CProjectile> {
	using Handle = uint32_t; // (id << 1) | synced
	static Handle ToHandle(const CProjectile* o);
};

template<typename TObject, typename TObjectSelector = ModelRenderContainerSelector<TObject>>
class ModelRenderContainer {
public:
	using Handle = typename ModelRenderContainerTraits<TObject>::Handle;
private:
	// note: there can be no more texture-types than S3DModel instances
	std::array< int, MAX_MODEL_OBJECTS > keys;
	std::vector< std::vector<Handle> > bins;

	size_t numObjs = 0;
	size_t numBins = 0;

	const TObjectSelector objectSelector;
private:
	int CalcObjectBinIdx(const TObject* o) const { return objectSelector(o); }
public:
	typedef  typename decltype(bins)::value_type  ObjectBin;
public:
	ModelRenderContainer(TObjectSelector objectSelector_)
		: numObjs{ 0 }
		, numBins{ 0 }
		, objectSelector{std::move(objectSelector_)}
	{
		bins.reserve(32);
		Clear();
	}

	ModelRenderContainer()
		: ModelRenderContainer(TObjectSelector{})
	{ }

	~ModelRenderContainer() {}

	void Clear() {
		keys.fill(0);

		// reuse inner vectors when reloading
		for (auto& bin : bins) {
			bin.clear();
		}

		numObjs = 0;
		numBins = 0;
	}

	void AddObject(const TObject* o) {
		const auto kb = keys.begin();
		const auto ke = keys.begin() + numBins;
		const auto ki = std::find(kb, ke, CalcObjectBinIdx(o));

		if (ki == ke)
			keys[numBins++] = CalcObjectBinIdx(o);

		if (bins.size() < numBins)
			bins.emplace_back();

		auto& bin = bins[ki - kb];

		if (bin.empty())
			bin.reserve(256);

		// numBins += (ki == ke);
		numObjs += spring::VectorInsertUnique(bin, ModelRenderContainerTraits<TObject>::ToHandle(o));
	}

	void DelObject(const TObject* o) {
		const auto kb = keys.begin();
		const auto ke = keys.begin() + numBins;
		const auto ki = std::find(kb, ke, CalcObjectBinIdx(o));

		if (ki == ke)
			return;

		auto& bin = bins[ki - kb];

		// object can be legally absent from this container
		// e.g. UnitDrawer invokes DelObject on both opaque
		// and alpha containers (since it does not know the
		// cloaked state) which also means the tex-type key
		// might not exist here
		numObjs -= spring::VectorErase(bin, ModelRenderContainerTraits<TObject>::ToHandle(o));
		numBins -= (bin.empty());

		if (!bin.empty())
			return;

		// keep empty bin, just remove it from the key-set
		std::swap(bins[ki - kb], bins[ke - 1 - kb]);
		std::swap(*ki, *(ke - 1));
	}

	bool empty() const { return numObjs == 0; }
	unsigned int GetNumObjects() const { return numObjs; }
	unsigned int GetNumObjectBins() const { return numBins; }
	unsigned int GetObjectBinKey(unsigned int idx) const { return keys[idx]; }

	const ObjectBin& GetObjectBin(unsigned int idx) const { return bins[idx]; }
};
