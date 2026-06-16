#include "PrimitiveMeshFactory.h"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace Asset
{
	namespace
	{
		constexpr uint32_t kSphereSlices = 32;
		constexpr uint32_t kSphereStacks = 16;
		constexpr uint32_t kCapsuleSlices = 32;
		constexpr uint32_t kCapsuleHemisphereRings = 8;

		[[nodiscard]] StaticMeshVertex MakeVertex(
			float x,
			float y,
			float z,
			float nx,
			float ny,
			float nz,
			float u,
			float v)
		{
			StaticMeshVertex vertex;
			vertex.Position = { x, y, z };
			vertex.Normal = { nx, ny, nz };
			vertex.TexCoord = { u, v };
			vertex.Color = { 1.0f, 1.0f, 1.0f, 1.0f };
			return vertex;
		}

		void AddTriangle(StaticMeshAsset& mesh, uint32_t a, uint32_t b, uint32_t c)
		{
			mesh.Indices.push_back(a);
			mesh.Indices.push_back(b);
			mesh.Indices.push_back(c);
		}

		void AddQuad(StaticMeshAsset& mesh, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
		{
			AddTriangle(mesh, a, b, c);
			AddTriangle(mesh, a, c, d);
		}

		void AddDefaultMaterialAndSubmesh(StaticMeshAsset& mesh, std::string_view name)
		{
			StaticMeshMaterial material;
			material.Name = std::string(name);
			material.DiffuseColor = { 1.0f, 1.0f, 1.0f, 1.0f };
			material.ImportedDiffuseTint = material.DiffuseColor;
			material.UseVertexColor = true;
			mesh.Materials.push_back(std::move(material));

			StaticMeshSubmesh submesh;
			submesh.VertexOffset = 0;
			submesh.VertexCount = static_cast<uint32_t>(mesh.Vertices.size());
			submesh.IndexOffset = 0;
			submesh.IndexCount = static_cast<uint32_t>(mesh.Indices.size());
			submesh.MaterialIndex = 0;
			submesh.Name = std::string(name);
			mesh.Submeshes.push_back(std::move(submesh));
			mesh.BindPoseVertices = mesh.Vertices;
		}

		[[nodiscard]] std::unique_ptr<StaticMeshAsset> CreatePlaneMesh()
		{
			auto mesh = std::make_unique<StaticMeshAsset>();
			mesh->PrimitiveKind = PrimitiveMeshKind::Plane;
			mesh->Vertices = {
				MakeVertex(-0.5f, 0.0f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f),
				MakeVertex(-0.5f, 0.0f,  0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f),
				MakeVertex( 0.5f, 0.0f,  0.5f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f),
				MakeVertex( 0.5f, 0.0f, -0.5f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f)
			};
			AddQuad(*mesh, 0, 1, 2, 3);
			AddDefaultMaterialAndSubmesh(*mesh, "Plane");
			return mesh;
		}

		void AddCubeFace(
			StaticMeshAsset& mesh,
			const DirectX::XMFLOAT3& normal,
			const std::array<DirectX::XMFLOAT3, 4>& positions)
		{
			const uint32_t baseIndex = static_cast<uint32_t>(mesh.Vertices.size());
			mesh.Vertices.push_back(MakeVertex(positions[0].x, positions[0].y, positions[0].z, normal.x, normal.y, normal.z, 0.0f, 1.0f));
			mesh.Vertices.push_back(MakeVertex(positions[1].x, positions[1].y, positions[1].z, normal.x, normal.y, normal.z, 0.0f, 0.0f));
			mesh.Vertices.push_back(MakeVertex(positions[2].x, positions[2].y, positions[2].z, normal.x, normal.y, normal.z, 1.0f, 0.0f));
			mesh.Vertices.push_back(MakeVertex(positions[3].x, positions[3].y, positions[3].z, normal.x, normal.y, normal.z, 1.0f, 1.0f));
			AddQuad(mesh, baseIndex, baseIndex + 1, baseIndex + 2, baseIndex + 3);
		}

		[[nodiscard]] std::unique_ptr<StaticMeshAsset> CreateCubeMesh()
		{
			auto mesh = std::make_unique<StaticMeshAsset>();
			mesh->PrimitiveKind = PrimitiveMeshKind::Cube;
			constexpr float h = 0.5f;
			AddCubeFace(*mesh, {  0.0f,  0.0f, -1.0f }, { { { -h, -h, -h }, { -h,  h, -h }, {  h,  h, -h }, {  h, -h, -h } } });
			AddCubeFace(*mesh, {  0.0f,  0.0f,  1.0f }, { { {  h, -h,  h }, {  h,  h,  h }, { -h,  h,  h }, { -h, -h,  h } } });
			AddCubeFace(*mesh, { -1.0f,  0.0f,  0.0f }, { { { -h, -h,  h }, { -h,  h,  h }, { -h,  h, -h }, { -h, -h, -h } } });
			AddCubeFace(*mesh, {  1.0f,  0.0f,  0.0f }, { { {  h, -h, -h }, {  h,  h, -h }, {  h,  h,  h }, {  h, -h,  h } } });
			AddCubeFace(*mesh, {  0.0f,  1.0f,  0.0f }, { { { -h,  h, -h }, { -h,  h,  h }, {  h,  h,  h }, {  h,  h, -h } } });
			AddCubeFace(*mesh, {  0.0f, -1.0f,  0.0f }, { { { -h, -h,  h }, { -h, -h, -h }, {  h, -h, -h }, {  h, -h,  h } } });
			AddDefaultMaterialAndSubmesh(*mesh, "Cube");
			return mesh;
		}

		[[nodiscard]] std::unique_ptr<StaticMeshAsset> CreateSphereMesh()
		{
			auto mesh = std::make_unique<StaticMeshAsset>();
			mesh->PrimitiveKind = PrimitiveMeshKind::Sphere;
			constexpr float radius = 0.5f;
			for (uint32_t stack = 0; stack <= kSphereStacks; ++stack)
			{
				const float v = static_cast<float>(stack) / static_cast<float>(kSphereStacks);
				const float phi = v * DirectX::XM_PI;
				const float y = std::cos(phi);
				const float ringRadius = std::sin(phi);
				for (uint32_t slice = 0; slice <= kSphereSlices; ++slice)
				{
					const float u = static_cast<float>(slice) / static_cast<float>(kSphereSlices);
					const float theta = u * DirectX::XM_2PI;
					const float x = ringRadius * std::cos(theta);
					const float z = ringRadius * std::sin(theta);
					mesh->Vertices.push_back(MakeVertex(x * radius, y * radius, z * radius, x, y, z, u, v));
				}
			}

			const uint32_t row = kSphereSlices + 1;
			for (uint32_t stack = 0; stack < kSphereStacks; ++stack)
			{
				for (uint32_t slice = 0; slice < kSphereSlices; ++slice)
				{
					const uint32_t a = stack * row + slice;
					const uint32_t b = a + row;
					AddQuad(*mesh, a, b, b + 1, a + 1);
				}
			}
			AddDefaultMaterialAndSubmesh(*mesh, "Sphere");
			return mesh;
		}

		[[nodiscard]] std::unique_ptr<StaticMeshAsset> CreateCapsuleMesh()
		{
			auto mesh = std::make_unique<StaticMeshAsset>();
			mesh->PrimitiveKind = PrimitiveMeshKind::Capsule;
			constexpr float radius = 0.35f;
			constexpr float halfCylinderHeight = 0.45f;
			const uint32_t rings = kCapsuleHemisphereRings * 2 + 1;

			for (uint32_t ring = 0; ring <= rings; ++ring)
			{
				const float t = static_cast<float>(ring) / static_cast<float>(rings);
				const float angle = -DirectX::XM_PIDIV2 + t * DirectX::XM_PI;
				const float normalY = std::sin(angle);
				const float ringRadius = std::cos(angle);
				const float centerY = normalY >= 0.0f ? halfCylinderHeight : -halfCylinderHeight;
				const float y = centerY + normalY * radius;

				for (uint32_t slice = 0; slice <= kCapsuleSlices; ++slice)
				{
					const float u = static_cast<float>(slice) / static_cast<float>(kCapsuleSlices);
					const float theta = u * DirectX::XM_2PI;
					const float normalX = ringRadius * std::cos(theta);
					const float normalZ = ringRadius * std::sin(theta);
					mesh->Vertices.push_back(MakeVertex(
						normalX * radius,
						y,
						normalZ * radius,
						normalX,
						normalY,
						normalZ,
						u,
						t));
				}
			}

			const uint32_t row = kCapsuleSlices + 1;
			for (uint32_t ring = 0; ring < rings; ++ring)
			{
				for (uint32_t slice = 0; slice < kCapsuleSlices; ++slice)
				{
					const uint32_t a = ring * row + slice;
					const uint32_t b = a + row;
					AddQuad(*mesh, a, b, b + 1, a + 1);
				}
			}
			AddDefaultMaterialAndSubmesh(*mesh, "Capsule");
			return mesh;
		}
	}

	std::string_view PrimitiveMeshKindToString(PrimitiveMeshKind kind) noexcept
	{
		switch (kind)
		{
		case PrimitiveMeshKind::Cube:
			return "Cube";
		case PrimitiveMeshKind::Sphere:
			return "Sphere";
		case PrimitiveMeshKind::Capsule:
			return "Capsule";
		case PrimitiveMeshKind::Plane:
			return "Plane";
		default:
			return "None";
		}
	}

	PrimitiveMeshKind PrimitiveMeshKindFromString(std::string_view text) noexcept
	{
		if (text == "Cube")
		{
			return PrimitiveMeshKind::Cube;
		}
		if (text == "Sphere")
		{
			return PrimitiveMeshKind::Sphere;
		}
		if (text == "Capsule")
		{
			return PrimitiveMeshKind::Capsule;
		}
		if (text == "Plane")
		{
			return PrimitiveMeshKind::Plane;
		}
		return PrimitiveMeshKind::None;
	}

	std::unique_ptr<StaticMeshAsset> CreatePrimitiveMesh(PrimitiveMeshKind kind)
	{
		switch (kind)
		{
		case PrimitiveMeshKind::Cube:
			return CreateCubeMesh();
		case PrimitiveMeshKind::Sphere:
			return CreateSphereMesh();
		case PrimitiveMeshKind::Capsule:
			return CreateCapsuleMesh();
		case PrimitiveMeshKind::Plane:
			return CreatePlaneMesh();
		default:
			return nullptr;
		}
	}
}
