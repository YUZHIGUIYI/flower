#include "static_mesh.h"
#include "../scene_graph.h"
#include "../scene_node.h"
#include <asset/asset_system.h>

#include "../editor/editor.h"

namespace engine
{
	constexpr size_t kMinSubMeshNumStartParallel = 100;

	StaticMeshComponent::~StaticMeshComponent()
	{

	}

	void StaticMeshComponent::tick(const RuntimeModuleTickData& tickData)
	{
		if (nullptr == m_cacheGPUMeshAsset)
		{
			// No cache GPUMeshAsset, need update.
			auto tempStore = m_staticMeshUUID;
			m_staticMeshUUID = {};
			setMesh(tempStore, m_staticMeshAssetRelativeRoot, m_bEngineAsset);
		}
		else
		{
			updateObjectCollectInfo(&tickData);
		}
	}

	void StaticMeshComponent::renderObjectCollect(std::vector<GPUStaticMeshPerObjectData>& collector)
	{
		const size_t objectOffsetId = collector.size();

		math::mat4 modelMatrix = getNode()->getTransform()->getWorldMatrix();
		math::mat4 modelMatrixPrev = getNode()->getTransform()->getPrevWorldMatrix();

		const bool bSelected = Editor::get()->getSceneNodeSelections().isSelected(SceneNodeSelctor(getNode()));

		auto updateObject = [&](GPUStaticMeshPerObjectData& object)
		{
			object.modelMatrix = modelMatrix;
			object.modelMatrixPrev = modelMatrixPrev;
			object.bSelected = bSelected;
		};

		if (m_perobjectCache.cachePerObjectData.size() > kMinSubMeshNumStartParallel)
		{
			const auto loop = [&updateObject, this](const size_t loopStart, const size_t loopEnd)
			{
				for (size_t i = loopStart; i < loopEnd; ++i)
				{
					updateObject(m_perobjectCache.cachePerObjectData[i]);
				}
			};
			ThreadPool::getDefault()->parallelizeLoop(0, m_perobjectCache.cachePerObjectData.size(), loop).wait();
		}
		else
		{
			for (size_t i = 0; i < m_perobjectCache.cachePerObjectData.size(); i++)
			{
				updateObject(m_perobjectCache.cachePerObjectData[i]);
			}
		}

		collector.insert(collector.end(),
			m_perobjectCache.cachePerObjectData.begin(),
			m_perobjectCache.cachePerObjectData.end());
	}

	bool StaticMeshComponent::setMesh(const UUID& in, const std::string& staticMeshAssetRelativeRoot, bool bEngineAsset)
	{
		if (
			in != m_staticMeshUUID || 
			staticMeshAssetRelativeRoot != m_staticMeshAssetRelativeRoot || 
			bEngineAsset != m_bEngineAsset)
		{
			// Update uuid and other infos.
			m_staticMeshUUID = in;
			m_staticMeshAssetRelativeRoot = staticMeshAssetRelativeRoot;
			m_bEngineAsset = bEngineAsset;

			// Clear static mesh cache.
			clearCache();

			if (!bEngineAsset)
			{
				m_cacheStaticMeshAsset = std::dynamic_pointer_cast<AssetStaticMesh>(getAssetSystem()->getAsset(m_staticMeshUUID));
			}

			// Get gpu asset.
			updateObjectCollectInfo(nullptr);
			return true;
		}

		return false;
	}

	void StaticMeshComponent::clearCache()
	{
		m_bMeshReplace = true;
		m_bMeshReady = false;
		m_cacheGPUMeshAsset = nullptr;
		m_cacheStaticMeshAsset = {};
		m_perobjectCache.clear();
		m_cachePerObjectMaterials.clear();
	}

	void StaticMeshComponent::updateObjectCollectInfo(const RuntimeModuleTickData* tickData)
	{
		if (m_staticMeshUUID.empty())
		{
			// No set mesh, return.
			return;
		}

		// If asset no load yet, add load task.
		if (m_cacheGPUMeshAsset == nullptr)
		{
			loadAssetByUUID();
		}

		if (tickData && (tickData->tickCount % 11 == 0)) // Try update after some frame.
		{
			// Update load state change cases.
			asyncLoadStateHandle();
			updateMaterials();
		}
	}

	void StaticMeshComponent::asyncLoadStateHandle()
	{
		// Pre-return if no mesh replace and no mesh loading state change.
		if (!m_bMeshReplace && m_bMeshReady)
		{
			return;
		}

		static const VkTransformMatrixKHR kInitTransformMatrix =
		{
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f
		};

		// Update mesh loading state.
		// If mesh replace, update proxys.
		// If mesh loading state change, upadte proxy.
		m_bMeshReady = m_cacheGPUMeshAsset->isAssetReady();
		if (m_bMeshReplace || m_bMeshReady)
		{
			GPUStaticMeshAsset* gpuAsset = m_cacheGPUMeshAsset->getReadyAsset<GPUStaticMeshAsset>();

			m_perobjectCache.clear();
			m_cachePerObjectMaterials.clear();

			// Collect object.
			if (!gpuAsset->isEngineAsset())
			{
				auto meshAsset = m_cacheStaticMeshAsset.lock();

				// Exist asset state.
				const auto& submeshes = meshAsset->getSubMeshes();
				m_perobjectCache.resize(submeshes.size());
				for (size_t i = 0; i < submeshes.size(); i++)
				{
					const auto& submesh = submeshes[i];
					auto& cacheObject = m_perobjectCache.cachePerObjectData.at(i);
					auto& cacheMaterialId = m_perobjectCache.cacheMaterialId.at(i);

					cacheObject.tangentsArrayId = gpuAsset->getTangentsBindless();
					cacheObject.uv0sArrayId = gpuAsset->getUv0sBindless();
					cacheObject.normalsArrayId = gpuAsset->getNormalsBindless();
					cacheObject.indicesArrayId = gpuAsset->getIndicesBindless();
					cacheObject.positionsArrayId = gpuAsset->getPositionBindless();
					cacheObject.indexStartPosition = submesh.indicesStart;
					cacheObject.indexCount = submesh.indicesCount;
					cacheObject.sphereBounds = math::vec4(submesh.bounds.origin, submesh.bounds.radius);
					cacheObject.extents = submesh.bounds.extents;
					cacheObject.objectId = getNode()->getId();

					if (auto material = std::dynamic_pointer_cast<StandardPBRMaterial>(getAssetSystem()->getAsset(submesh.material)))
					{
						// Store Material header UUID.
						cacheMaterialId = material->getUUID(); 

						// Insert material if no exist cache.
						auto& cacheMaterialPair = m_cachePerObjectMaterials[cacheMaterialId];

						cacheMaterialPair.handle = material->buildCache();
						cacheMaterialPair.asset = material;

						// init material.
						cacheObject.material = material->getGPUOnly();
					}
					else
					{
						cacheObject.material = GPUMaterialStandardPBR::getDefault();
						LOG_WARN("Missing material, used default for submesh.");
					}
				}
			}
			else
			{
				// No exist asset.
				GPUStaticMeshPerObjectData object{};
				object.tangentsArrayId = gpuAsset->getTangentsBindless();
				object.uv0sArrayId = gpuAsset->getUv0sBindless();
				object.normalsArrayId = gpuAsset->getNormalsBindless();
				object.indicesArrayId = gpuAsset->getIndicesBindless();
				object.positionsArrayId = gpuAsset->getPositionBindless();
				object.material = GPUMaterialStandardPBR::getDefault();

				// Default mesh, use fallback.
				object.indexStartPosition = 0;
				object.indexCount = (uint32_t)gpuAsset->getIndicesCount();
				const auto& renderBounds = getContext()->getEngineMeshRenderBounds(gpuAsset->getAssetUUID());
				object.sphereBounds = math::vec4(renderBounds.origin, renderBounds.radius);
				object.extents = renderBounds.extents;

				object.objectId = getNode()->getId();

				m_perobjectCache.resize(1);
				m_perobjectCache.cachePerObjectData[0] = std::move(object);
			}
		}

		// We handle once mesh replace event.
		m_bMeshReplace = false;
	}

	void StaticMeshComponent::updateMaterials()
	{
		// Only asset mesh need load and lazy update, engine mesh all ready.
		if (!m_cacheStaticMeshAsset.lock())
		{
			return;
		}

		for (auto& materialPair : m_cachePerObjectMaterials)
		{
			auto& material = materialPair.second.asset;
			material->getAndTryBuildGPU();
		}

		if (m_perobjectCache.cachePerObjectData.size() > kMinSubMeshNumStartParallel)
		{
			const auto loop = [this](const size_t loopStart, const size_t loopEnd)
			{
				for (size_t i = loopStart; i < loopEnd; ++i)
				{
					auto& object = m_perobjectCache.cachePerObjectData[i];
					const auto& id = m_perobjectCache.cacheMaterialId[i];
					const auto& material = m_cachePerObjectMaterials.at(id).asset;

					object.material = material->getGPUOnly();
				}
			};
			ThreadPool::getDefault()->parallelizeLoop(0, m_perobjectCache.cachePerObjectData.size(), loop).wait();
		}
		else
		{
			for (size_t i = 0; i < m_perobjectCache.cachePerObjectData.size(); i++)
			{
				auto& object = m_perobjectCache.cachePerObjectData[i];
				const auto& id = m_perobjectCache.cacheMaterialId[i];

				if (!id.empty())
				{
					const auto& material = m_cachePerObjectMaterials.at(id).asset;
					object.material = material->getGPUOnly();
				}
			}
		}
	}

	void StaticMeshComponent::loadAssetByUUID()
	{
		m_cacheGPUMeshAsset = getContext()->getOrCreateStaticMeshAsset(m_staticMeshUUID);
		m_bMeshReplace = true;
		m_bMeshReady = m_cacheGPUMeshAsset->isAssetReady();
	}

}
