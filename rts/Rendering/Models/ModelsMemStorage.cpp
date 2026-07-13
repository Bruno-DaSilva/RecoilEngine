#include "ModelsMemStorage.h"
#include "Sim/Features/Feature.h"
#include "Sim/Units/Unit.h"

#include "System/Misc/TracyDefs.h"

TransformsMemStorage transformsMemStorage;
ModelUniformsStorage modelUniformsStorage;

////////////////////////////////////////////////////////////////////

void ModelUniformsStorage::Init()
{
	assert(updateList.Empty());
	assert(objectsMaps[OBJ_UNIT].empty() && objectsMaps[OBJ_FEATURE].empty());
	assert(storage.empty());

	// reserve slot 0 as the shared dummy element (INVALID_INDEX)
	storage[AddSlot()] = dummy;
}

void ModelUniformsStorage::Kill()
{
	// Remaining objects are not cleared anywhere (not a good thing) so delete them here
	updateList.Clear();
	storage.clear();

	for (auto& objectsMap : objectsMaps)
		objectsMap.clear();
}

size_t ModelUniformsStorage::AddSlot()
{
	const size_t idx = storage.Add(ModelUniformData());

	if (storage.size() > updateList.Size()) {
		//new item got added to the end of storage
		updateList.EmplaceBackUpdate();
	} else {
		// storage got updated somewhere in the middle, use updateList.SetUpdate()
		updateList.SetUpdate(idx);
	}
	assert(storage.size() == updateList.Size());

	return idx;
}

size_t ModelUniformsStorage::AddObjectImpl(ObjKind kind, int id)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const size_t idx = AddSlot();
	objectsMaps[kind][id] = idx;

	return idx;
}

void ModelUniformsStorage::DelObjectImpl(ObjKind kind, int id)
{
	RECOIL_DETAILED_TRACY_ZONE;
	auto& objectsMap = objectsMaps[kind];
	const auto it = objectsMap.find(id);

	if (it == objectsMap.end())
		return;

	storage.Del(it->second);

	if (storage.size() < updateList.Size()) {
		// storage got one element shorter, trim updateList as well
		updateList.Trim(it->second);
	} else {
		// storage got updated somewhere in the middle, use updateList.SetUpdate()
		updateList.SetUpdate(it->second);
	}

	assert(storage.size() == updateList.Size());

	objectsMap.erase(it);
}

size_t ModelUniformsStorage::GetObjOffsetImpl(ObjKind kind, int id)
{
	RECOIL_DETAILED_TRACY_ZONE;
	const auto& objectsMap = objectsMaps[kind];
	const auto it = objectsMap.find(id);
	if (it != objectsMap.end())
		return it->second;

	return AddObjectImpl(kind, id);
}

size_t ModelUniformsStorage::GetObjOffsetImpl(ObjKind kind, int id) const
{
	RECOIL_DETAILED_TRACY_ZONE;
	const auto& objectsMap = objectsMaps[kind];
	const auto it = objectsMap.find(id);
	if (it != objectsMap.end())
		return it->second;

	return INVALID_INDEX;
}

size_t ModelUniformsStorage::AddObject(const CUnit* o)    { return AddObjectImpl(OBJ_UNIT   , o->id); }
size_t ModelUniformsStorage::AddObject(const CFeature* o) { return AddObjectImpl(OBJ_FEATURE, o->id); }

void ModelUniformsStorage::DelObject(const CUnit* o)    { DelObjectImpl(OBJ_UNIT   , o->id); }
void ModelUniformsStorage::DelObject(const CFeature* o) { DelObjectImpl(OBJ_FEATURE, o->id); }

size_t ModelUniformsStorage::GetObjOffset(const CUnit* o)          { return GetObjOffsetImpl(OBJ_UNIT   , o->id); }
size_t ModelUniformsStorage::GetObjOffset(const CFeature* o)       { return GetObjOffsetImpl(OBJ_FEATURE, o->id); }
size_t ModelUniformsStorage::GetObjOffset(const CUnit* o) const    { return GetObjOffsetImpl(OBJ_UNIT   , o->id); }
size_t ModelUniformsStorage::GetObjOffset(const CFeature* o) const { return GetObjOffsetImpl(OBJ_FEATURE, o->id); }

const ModelUniformsStorage::MyType& ModelUniformsStorage::GetObjUniformsArray(const CUnit* o) const    { return storage[GetObjOffset(o)]; }
const ModelUniformsStorage::MyType& ModelUniformsStorage::GetObjUniformsArray(const CFeature* o) const { return storage[GetObjOffset(o)]; }

ModelUniformsStorage::MyType& ModelUniformsStorage::GetObjUniformsArray(const CUnit* o)
{
	const size_t offset = GetObjOffset(o);
	updateList.SetUpdate(offset);
	return storage[offset];
}

ModelUniformsStorage::MyType& ModelUniformsStorage::GetObjUniformsArray(const CFeature* o)
{
	const size_t offset = GetObjOffset(o);
	updateList.SetUpdate(offset);
	return storage[offset];
}

////////////////////////////////////////////////////////////////////

TransformsMemStorage::TransformsMemStorage()
	: storage(StablePosAllocator<MyType>(INIT_NUM_ELEMS))
	, updateList(INIT_NUM_ELEMS)
{}

void TransformsMemStorage::Reset()
{
	assert(Threading::IsMainThread());
	storage.Reset();
	updateList.Clear();
}

size_t TransformsMemStorage::Allocate(size_t numElems)
{
	auto lock = CModelsLock::GetScopedLock();

	auto res = storage.Allocate(numElems);
	updateList.Resize(storage.GetSize());

	return res;
}

void TransformsMemStorage::Free(size_t firstElem, size_t numElems, const MyType* T0)
{
	auto lock = CModelsLock::GetScopedLock();

	storage.Free(firstElem, numElems, T0);
	updateList.SetUpdate(firstElem, numElems);
	updateList.Trim(storage.GetSize());
}

const TransformsMemStorage::MyType& TransformsMemStorage::operator[](std::size_t idx) const
{
	auto lock = CModelsLock::GetScopedLock();

	return storage[idx];
}