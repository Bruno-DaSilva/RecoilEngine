#pragma once

#include <memory>
#include <vector>

#include "ModelsMemStorageDefs.h"
#include "ModelsLock.h"
#include "Game/BoundaryStats.h"
#include "System/Transform.hpp"
#include "System/MemPoolTypes.h"
#include "System/FreeListMap.h"
#include "System/UnorderedMap.hpp"
#include "System/TypeToStr.h"
#include "System/Threading/SpringThreading.h"
#include "Sim/Misc/GlobalConstants.h"
#include "Sim/Objects/SolidObjectDef.h"
#include "Rendering/Common/UpdateList.h"

class TransformsMemStorage {
public:
	using MyType = Transform;
	using EqualCmpFunctor = bool(*)(const MyType&, const MyType&);
public:
	explicit TransformsMemStorage();
	void Reset();

	size_t Allocate(size_t numElems);

	void Free(size_t firstElem, size_t numElems, const MyType* T0 = nullptr);

	const auto& GetData() const { return storage.GetData(); }
	const auto  GetSize() const { return storage.GetSize(); }

	template<typename MyTypeLike = MyType> // to force universal references
	bool UpdateIfChanged(std::size_t idx, MyTypeLike&& newValue, EqualCmpFunctor eqCmp) {
		auto lock = CModelsLock::GetScopedLock();

		using DT = StablePosAllocator<MyType>;
		BoundaryStats::Add(BoundaryStats::ctr.traChecked);
		const auto& curValue = const_cast<const DT&>(storage)[idx];
		if (eqCmp(curValue, newValue))
			return false;

		BoundaryStats::Add(BoundaryStats::ctr.traChanged);
		updateList.SetUpdate(idx);
		auto& mutValue = const_cast<DT&>(storage)[idx];
		mutValue = newValue;

		assert(updateList.Size() == storage.GetSize());

		return true;
	}

	template<typename MyTypeLike = MyType> // to force universal references
	void UpdateForced(std::size_t idx, MyTypeLike&& newValue) {
		auto lock = CModelsLock::GetScopedLock();

		BoundaryStats::Add(BoundaryStats::ctr.traForced);
		updateList.SetUpdate(idx);
		auto& mutValue = storage[idx];
		mutValue = newValue;

		assert(updateList.Size() == storage.GetSize());
	}

	const MyType& operator[](std::size_t idx) const;

	const auto& GetUpdateList() const { return updateList; }
	      auto& GetUpdateList()       { return updateList; }
private:
	StablePosAllocator<MyType> storage;
	UpdateList updateList;
private:
	static constexpr int INIT_NUM_ELEMS = 1 << 16u;
public:
	static constexpr auto INVALID_INDEX = StablePosAllocator<MyType>::INVALID_INDEX;
};

extern TransformsMemStorage transformsMemStorage;


////////////////////////////////////////////////////////////////////

class ScopedTransformMemAlloc {
public:
	ScopedTransformMemAlloc() : ScopedTransformMemAlloc(0u) {};
	ScopedTransformMemAlloc(std::size_t numElems_)
		: numElems{numElems_}
	{
		firstElem = transformsMemStorage.Allocate(numElems);
	}

	ScopedTransformMemAlloc(const ScopedTransformMemAlloc&) = delete;
	ScopedTransformMemAlloc(ScopedTransformMemAlloc&& smma) noexcept { *this = std::move(smma); }

	~ScopedTransformMemAlloc() {
		if (firstElem == TransformsMemStorage::INVALID_INDEX)
			return;

		transformsMemStorage.Free(firstElem, numElems, &Transform::Zero());
	}

	bool Valid() const { return firstElem != TransformsMemStorage::INVALID_INDEX;	}
	std::size_t GetOffset(bool assertInvalid = true) const {
		if (assertInvalid)
			assert(Valid());

		return firstElem;
	}

	ScopedTransformMemAlloc& operator= (const ScopedTransformMemAlloc&) = delete;
	ScopedTransformMemAlloc& operator= (ScopedTransformMemAlloc&& smma) noexcept {
		//swap to prevent dealloc on dying object, yet enable destructor to do its thing on valid object
		std::swap(firstElem, smma.firstElem);
		std::swap(numElems , smma.numElems );

		return *this;
	}

	const auto& operator[](std::size_t offset) const {
		assert(firstElem != TransformsMemStorage::INVALID_INDEX);
		assert(offset >= 0 && offset < numElems);

		return transformsMemStorage[firstElem + offset];
	}

	template<typename MyTypeLike = TransformsMemStorage::MyType> // to force universal references
	bool UpdateIfChanged(std::size_t offset, MyTypeLike&& newValue) {
		static const auto EqCmp = [](const TransformsMemStorage::MyType& lhs, const TransformsMemStorage::MyType& rhs) {
			return lhs.equals(rhs);
		};

		assert(firstElem != TransformsMemStorage::INVALID_INDEX);
		assert(offset >= 0 && offset < numElems);

		return transformsMemStorage.UpdateIfChanged(firstElem + offset, std::forward<MyTypeLike>(newValue), EqCmp);
	}

	template<typename MyTypeLike = TransformsMemStorage::MyType> // to force universal references
	void UpdateForced(std::size_t offset, MyTypeLike&& newValue) {
		assert(firstElem != TransformsMemStorage::INVALID_INDEX);
		assert(offset >= 0 && offset < numElems);

		transformsMemStorage.UpdateForced(firstElem + offset, std::forward<MyTypeLike>(newValue));
	}
public:
	static const ScopedTransformMemAlloc& Dummy() {
		static ScopedTransformMemAlloc dummy;

		return dummy;
	};
private:
	std::size_t firstElem = TransformsMemStorage::INVALID_INDEX;
	std::size_t numElems  = 0u;
};

////////////////////////////////////////////////////////////////////

class CUnit;
class CFeature;
// per-object shader uniforms storage. Keyed by object id per kind (PR 14:
// drawer-side containers hold IDs, not sim-object pointers); only units and
// features are ever registered (by CModelDrawerDataBase add/del), the def-
// and model-typed overloads below are no-op dummies for template callers.
class ModelUniformsStorage {
private:
	using MyType = ModelUniformData;
public:
	void Init();
	void Kill();
public:
	size_t AddObject(const CUnit* o);
	size_t AddObject(const CFeature* o);
	size_t GetObjOffset(const CUnit* o);
	size_t GetObjOffset(const CFeature* o);
	size_t GetObjOffset(const CUnit* o) const;
	size_t GetObjOffset(const CFeature* o) const;
	const MyType& GetObjUniformsArray(const CUnit* o) const;
	const MyType& GetObjUniformsArray(const CFeature* o) const;
	MyType& GetObjUniformsArray(const CUnit* o);
	MyType& GetObjUniformsArray(const CFeature* o);
	void   DelObject(const CUnit* o);
	void   DelObject(const CFeature* o);

	size_t AddObject(const SolidObjectDef* o) { return INVALID_INDEX; }
	size_t GetObjOffset(const SolidObjectDef* o) { return INVALID_INDEX; }
	const MyType& GetObjUniformsArray(const SolidObjectDef* o) const { return dummy; }
	MyType& GetObjUniformsArray(const SolidObjectDef* o) { return dummy; }
	void   DelObject(const SolidObjectDef* o) {}

	size_t AddObject(const S3DModel* o) { return INVALID_INDEX; }
	size_t GetObjOffset(const S3DModel* o) { return INVALID_INDEX; }
	const MyType& GetObjUniformsArray(const S3DModel* o) const { return dummy; }
	MyType& GetObjUniformsArray(const S3DModel* o) { return dummy; }
	void   DelObject(const S3DModel* o) {}

	auto GetSize() const { return storage.GetData().size(); }
	const auto& GetData() const { return storage.GetData(); }

	const auto& GetUpdateList() const { return updateList; }
	      auto& GetUpdateList()       { return updateList; }
private:
	enum ObjKind : uint8_t { OBJ_UNIT = 0, OBJ_FEATURE = 1, OBJ_KIND_CNT = 2 };

	size_t AddSlot();
	size_t AddObjectImpl(ObjKind kind, int id);
	size_t GetObjOffsetImpl(ObjKind kind, int id);
	size_t GetObjOffsetImpl(ObjKind kind, int id) const;
	void   DelObjectImpl(ObjKind kind, int id);

	UpdateList updateList;
public:
	static constexpr size_t INVALID_INDEX = 0;
private:
	inline static MyType dummy = {};

	// object id -> storage offset, per object kind
	spring::unordered_map<int, size_t> objectsMaps[OBJ_KIND_CNT];
	spring::FreeListMap<MyType> storage;
};

extern ModelUniformsStorage modelUniformsStorage;
