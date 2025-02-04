/*
* Vulkan Example - glTF scene loading and rendering
*
* Copyright (C) 2020-2022 by Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/

/*
 * Shows how to load and display a simple scene from a glTF file
 * Note that this isn't a complete glTF loader and only basic functions are shown here
 * This means no complex materials, no animations, no skins, etc.
 * For details on how glTF 2.0 works, see the official spec at https://github.com/KhronosGroup/glTF/tree/master/specification/2.0
 *
 * Other samples will load models using a dedicated model loader with more features (see base/VulkanglTFModel.hpp)
 *
 * If you are looking for a complete glTF implementation, check out https://github.com/SaschaWillems/Vulkan-glTF-PBR/
 */

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE
#ifdef VK_USE_PLATFORM_ANDROID_KHR
#define TINYGLTF_ANDROID_LOAD_FROM_ASSETS
#endif
#include "tiny_gltf.h"

#include "vulkanexamplebase.h"

#define ENABLE_VALIDATION true

 // Contains everything required to render a glTF model in Vulkan
 // This class is heavily simplified (compared to glTF's feature set) but retains the basic glTF structure
class VulkanglTFModel
{
public:
	// The class requires some Vulkan objects so it can create it's own resources
	vks::VulkanDevice* vulkanDevice;
	VkQueue copyQueue;

	// The vertex layout for the samples' model
	struct Vertex {
		glm::vec3 pos;
		glm::vec3 normal;
		glm::vec2 uv;
		glm::vec3 color;
	};

	// Single vertex buffer for all primitives
	struct {
		VkBuffer buffer;
		VkDeviceMemory memory;
	} vertices;

	// Single index buffer for all primitives
	struct {
		int count;
		VkBuffer buffer;
		VkDeviceMemory memory;
	} indices;

	// The following structures roughly represent the glTF scene structure
	// To keep things simple, they only contain those properties that are required for this sample
	//struct Node;

	// A primitive contains the data for a single draw call
	struct Primitive {
		uint32_t firstIndex;
		uint32_t indexCount;
		int32_t materialIndex;
	};

	// Contains the node's (optional) geometry and can be made up of an arbitrary number of primitives
	// 包含节点的（可选）几何信息，并且可以由任意数量的基元组成
	struct Mesh {
		std::vector<Primitive> primitives;
		struct UniformBuffer {
			vks::Buffer buffer;
			VkDescriptorSet descriptorSet;
			void* mapped;
		} uniformBuffer;
	};

	// A node represents an object in the glTF scene graph
	struct Node 
	{
		Node* parent;
		std::vector<Node*> children;
		Mesh mesh;
		glm::mat4 matrix;
		
		//Elysia
		uint32_t index;
		glm::vec3           translation{};
		glm::vec3           scale{ 1.0f };
		glm::quat           rotation{};
		
		glm::mat4 getLocalMatrix()
		{
			return glm::translate(glm::mat4(1.0f), translation) * glm::mat4(rotation) * glm::scale(glm::mat4(1.0f), scale) * matrix;
		}

		~Node() {
			for (auto& child : children) {
				delete child;
			}
		}
	};

	// A glTF material stores information in e.g. the texture that is attached to it and colors
	// 一个glTF材质存储了与其关联的纹理和颜色等信息
	struct Material {
		glm::vec4 baseColorFactor = glm::vec4(1.0f);
		uint32_t baseColorTextureIndex;
		//Elysia
		uint32_t metallicRoughnessTextureIndex;
		uint32_t normalTextureIndex;
	};

	// Contains the texture for a single glTF image
	// Images may be reused by texture objects and are as such separated
	// 包含单个 glTF 图像的纹理
	// 图像可能会被纹理对象重用，因此会被分开处理
	struct Image {
		vks::Texture2D texture;
		// We also store (and create) a descriptor set that's used to access this texture from the fragment shader
		// 我们还存储（和创建）一个描述符集，用于从片段着色器中访问该纹理
		VkDescriptorSet descriptorSet;
	};

	// A glTF texture stores a reference to the image and a sampler
	// In this sample, we are only interested in the image
	// 一个 glTF 纹理存储了对图像和采样器的引用
	// 在本示例中，我们只对图像感兴趣
	struct Texture {
		int32_t imageIndex;
	};

	// 动画相关
	// Elysia
	struct AnimationSampler
	{
		std::string interpolation;
		std::vector<float> inputs;
		std::vector<glm::vec4> outputsVec4;
	};

	struct AnimationChannel
	{
		std::string path;
		Node* node;
		uint32_t    samplerIndex;
	};

	struct Animation
	{
		std::string                   name;
		std::vector<AnimationSampler> samplers;
		std::vector<AnimationChannel> channels;
		float                         start = std::numeric_limits<float>::max();
		float                         end = std::numeric_limits<float>::min();
		float                         currentTime = 0.0f;
	};

	/*
		Model data
	*/
	std::vector<Image> images;
	std::vector<Texture> textures;
	std::vector<Material> materials;
	std::vector<Node*> nodes;
	//Elysia
	std::vector<Animation> animations;
	std::vector<Node*> linearMeshNodes;
	uint32_t activeAnimation = 0;

	//析构
	~VulkanglTFModel()
	{
		for (auto node : nodes) {
			delete node;
		}
		// Release all Vulkan resources allocated for the model
		vkDestroyBuffer(vulkanDevice->logicalDevice, vertices.buffer, nullptr);
		vkFreeMemory(vulkanDevice->logicalDevice, vertices.memory, nullptr);
		vkDestroyBuffer(vulkanDevice->logicalDevice, indices.buffer, nullptr);
		vkFreeMemory(vulkanDevice->logicalDevice, indices.memory, nullptr);
		for (Image image : images) {
			vkDestroyImageView(vulkanDevice->logicalDevice, image.texture.view, nullptr);
			vkDestroyImage(vulkanDevice->logicalDevice, image.texture.image, nullptr);
			vkDestroySampler(vulkanDevice->logicalDevice, image.texture.sampler, nullptr);
			vkFreeMemory(vulkanDevice->logicalDevice, image.texture.deviceMemory, nullptr);
		}
	}

	/*
		glTF loading functions

		The following functions take a glTF input model loaded via tinyglTF and convert all required data into our own structure
	*/

	void loadImages(tinygltf::Model& input)
	{
		// Images can be stored inside the glTF (which is the case for the sample model), so instead of directly
		// loading them from disk, we fetch them from the glTF loader and upload the buffers
		// 图像可以存储在glTF内部（对于示例模型是这种情况），因此我们不直接从磁盘加载它们，
		// 而是从glTF加载器中获取它们并上传缓冲区
		images.resize(input.images.size());
		for (size_t i = 0; i < input.images.size(); i++) {
			tinygltf::Image& glTFImage = input.images[i];
			// Get the image data from the glTF loader
			//从glTF加载器获取图像数据
			unsigned char* buffer = nullptr;
			VkDeviceSize bufferSize = 0;
			bool deleteBuffer = false;
			// We convert RGB-only images to RGBA, as most devices don't support RGB-formats in Vulkan
			//我们将仅包含RGB通道的图像转换为RGBA格式，因为大多数设备在Vulkan中不支持RGB格式
			
			//glTFImage.component的值为3，表示图像的通道数为RGB（没有透明通道）。
			if (glTFImage.component == 3) 
			{
				//计算新缓冲区的大小bufferSize，即宽度乘以高度乘以4。
				bufferSize = glTFImage.width * glTFImage.height * 4;
				//创建新的缓冲区buffer，并通过遍历每个像素将RGB值复制到新缓冲区的对应位置，同时在每个像素后面添加默认的透明通道值。
				buffer = new unsigned char[bufferSize];
				unsigned char* rgba = buffer;
				unsigned char* rgb = &glTFImage.image[0];
				for (size_t i = 0; i < glTFImage.width * glTFImage.height; ++i) {
					memcpy(rgba, rgb, sizeof(unsigned char) * 3);
					rgba += 4;
					rgb += 3;
				}
				//设置deleteBuffer标志为true，表示在不再需要时删除缓冲区。
				deleteBuffer = true;
			}
			else {
				buffer = &glTFImage.image[0];
				bufferSize = glTFImage.image.size();
			}
			// Load texture from image buffer
			// 从图像缓冲区加载图片
			images[i].texture.fromBuffer(buffer, bufferSize, VK_FORMAT_R8G8B8A8_UNORM, glTFImage.width, glTFImage.height, vulkanDevice, copyQueue);
			if (deleteBuffer) {
				delete[] buffer;
			}
		}
	}

	//每个纹理数据结构记录了它所使用的图像在模型的图像数组中的索引，以便后续的纹理加载和使用过程中可以正确地关联纹理和图像数据。
	void loadTextures(tinygltf::Model& input)
	{
		textures.resize(input.textures.size());
		for (size_t i = 0; i < input.textures.size(); i++) {
			textures[i].imageIndex = input.textures[i].source;
		}
	}
	void loadMaterials(tinygltf::Model& input)
	{
		materials.resize(input.materials.size());
		for (size_t i = 0; i < input.materials.size(); i++) {
			// We only read the most basic properties required for our sample
			tinygltf::Material glTFMaterial = input.materials[i];
			// Get the base color factor
			if (glTFMaterial.values.find("baseColorFactor") != glTFMaterial.values.end()) {
				materials[i].baseColorFactor = glm::make_vec4(glTFMaterial.values["baseColorFactor"].ColorFactor().data());
			}
			// Get base color texture index
			if (glTFMaterial.values.find("baseColorTexture") != glTFMaterial.values.end()) {
				materials[i].baseColorTextureIndex = glTFMaterial.values["baseColorTexture"].TextureIndex();
			}
			if (glTFMaterial.values.find("metallicRoughnessTexture") != glTFMaterial.values.end()) {
				materials[i].metallicRoughnessTextureIndex = glTFMaterial.values["metallicRoughnessTexture"].TextureIndex();
			}
			materials[i].normalTextureIndex = glTFMaterial.normalTexture.index;
		}
	}

	//Elysia
	void loadNode(const tinygltf::Node& inputNode, const tinygltf::Model& input, VulkanglTFModel::Node* parent, uint32_t nodeIndex, std::vector<uint32_t>& indexBuffer, std::vector<VulkanglTFModel::Vertex>& vertexBuffer)
	{
		// 创建一个新的节点
		VulkanglTFModel::Node* node = new VulkanglTFModel::Node{};
		node->matrix = glm::mat4(1.0f);
		node->parent = parent;
		node->index = nodeIndex;	

		// Get the local node matrix
		// It's either made up from translation, rotation, scale or a 4x4 matrix
		// 获取局部节点矩阵
		// 它要么由平移、旋转、缩放组成，要么由4x4矩阵组成
		if (inputNode.translation.size() == 3) {
			//node->matrix = glm::translate(node->matrix, glm::vec3(glm::make_vec3(inputNode.translation.data())));
			node->translation = glm::make_vec3(inputNode.translation.data());
		}
		if (inputNode.rotation.size() == 4) {
			//glm::quat q = glm::make_quat(inputNode.rotation.data());
			//node->matrix *= glm::mat4(q);
			node->rotation = glm::make_quat(inputNode.rotation.data());
		}
		if (inputNode.scale.size() == 3) {
			//node->matrix = glm::scale(node->matrix, glm::vec3(glm::make_vec3(inputNode.scale.data())));
			node->scale = glm::make_vec3(inputNode.scale.data());
		}
		if (inputNode.matrix.size() == 16) {
			//node->matrix = glm::make_mat4x4(inputNode.matrix.data());
			node->matrix = glm::make_mat4x4(inputNode.matrix.data());
		};

		// Load node's children
		// 加载节点的子节点
		if (inputNode.children.size() > 0) {
			for (size_t i = 0; i < inputNode.children.size(); i++) {
				loadNode(input.nodes[inputNode.children[i]], input , node, inputNode.children[i], indexBuffer, vertexBuffer);
			}
		}

		// If the node contains mesh data, we load vertices and indices from the buffers
		// In glTF this is done via accessors and buffer views
		// 如果节点包含网格数据，我们从缓冲区加载顶点和索引
		// 在glTF中，这是通过访问器和缓冲区视图来完成的
		if (inputNode.mesh > -1) {
			const tinygltf::Mesh mesh = input.meshes[inputNode.mesh];
			// Iterate through all primitives of this node's mesh
			// 遍历该节点的网格的所有原始体（primitive）
			for (size_t i = 0; i < mesh.primitives.size(); i++) {
				const tinygltf::Primitive& glTFPrimitive = mesh.primitives[i];
				uint32_t firstIndex = static_cast<uint32_t>(indexBuffer.size());
				uint32_t vertexStart = static_cast<uint32_t>(vertexBuffer.size());
				uint32_t indexCount = 0;
				// Vertices
				// 顶点
				{
					const float* positionBuffer = nullptr;
					const float* normalsBuffer = nullptr;
					const float* texCoordsBuffer = nullptr;
					size_t vertexCount = 0;

					// Get buffer data for vertex positions
					// 获取顶点位置的缓冲区数据
					if (glTFPrimitive.attributes.find("POSITION") != glTFPrimitive.attributes.end()) {
						const tinygltf::Accessor& accessor = input.accessors[glTFPrimitive.attributes.find("POSITION")->second];
						const tinygltf::BufferView& view = input.bufferViews[accessor.bufferView];
						positionBuffer = reinterpret_cast<const float*>(&(input.buffers[view.buffer].data[accessor.byteOffset + view.byteOffset]));
						vertexCount = accessor.count;
					}
					// Get buffer data for vertex normals
					// 获取顶点法线的缓冲区数据
					if (glTFPrimitive.attributes.find("NORMAL") != glTFPrimitive.attributes.end()) {
						const tinygltf::Accessor& accessor = input.accessors[glTFPrimitive.attributes.find("NORMAL")->second];
						const tinygltf::BufferView& view = input.bufferViews[accessor.bufferView];
						normalsBuffer = reinterpret_cast<const float*>(&(input.buffers[view.buffer].data[accessor.byteOffset + view.byteOffset]));
					}
					// Get buffer data for vertex texture coordinates
					// glTF supports multiple sets, we only load the first one
					
					//获取顶点纹理坐标的缓冲区数据
					//glTF支持多个集合，我们只加载第一个集合
					if (glTFPrimitive.attributes.find("TEXCOORD_0") != glTFPrimitive.attributes.end()) {
						const tinygltf::Accessor& accessor = input.accessors[glTFPrimitive.attributes.find("TEXCOORD_0")->second];
						const tinygltf::BufferView& view = input.bufferViews[accessor.bufferView];
						texCoordsBuffer = reinterpret_cast<const float*>(&(input.buffers[view.buffer].data[accessor.byteOffset + view.byteOffset]));
					}

					// Append data to model's vertex buffer
					// 将数据附加到模型的顶点缓冲区
					for (size_t v = 0; v < vertexCount; v++) {
						Vertex vert{};
						vert.pos = glm::vec4(glm::make_vec3(&positionBuffer[v * 3]), 1.0f);
						vert.normal = glm::normalize(glm::vec3(normalsBuffer ? glm::make_vec3(&normalsBuffer[v * 3]) : glm::vec3(0.0f)));
						vert.uv = texCoordsBuffer ? glm::make_vec2(&texCoordsBuffer[v * 2]) : glm::vec3(0.0f);
						vert.color = glm::vec3(1.0f);
						vertexBuffer.push_back(vert);
					}
				}
				// Indices
				// 索引
				{
					const tinygltf::Accessor& accessor = input.accessors[glTFPrimitive.indices];
					const tinygltf::BufferView& bufferView = input.bufferViews[accessor.bufferView];
					const tinygltf::Buffer& buffer = input.buffers[bufferView.buffer];

					indexCount += static_cast<uint32_t>(accessor.count);

					// glTF supports different component types of indices
					//glTF支持不同类型的索引组件


					switch (accessor.componentType) {
					case TINYGLTF_PARAMETER_TYPE_UNSIGNED_INT: {
						const uint32_t* buf = reinterpret_cast<const uint32_t*>(&buffer.data[accessor.byteOffset + bufferView.byteOffset]);
						for (size_t index = 0; index < accessor.count; index++) {
							indexBuffer.push_back(buf[index] + vertexStart);
						}
						break;
					}
					case TINYGLTF_PARAMETER_TYPE_UNSIGNED_SHORT: {
						const uint16_t* buf = reinterpret_cast<const uint16_t*>(&buffer.data[accessor.byteOffset + bufferView.byteOffset]);
						for (size_t index = 0; index < accessor.count; index++) {
							indexBuffer.push_back(buf[index] + vertexStart);
						}
						break;
					}
					case TINYGLTF_PARAMETER_TYPE_UNSIGNED_BYTE: {
						const uint8_t* buf = reinterpret_cast<const uint8_t*>(&buffer.data[accessor.byteOffset + bufferView.byteOffset]);
						for (size_t index = 0; index < accessor.count; index++) {
							indexBuffer.push_back(buf[index] + vertexStart);
						}
						break;
					}
					default:
						std::cerr << "Index component type " << accessor.componentType << " not supported!" << std::endl;
						return;
					}
				}
				Primitive primitive{};
				primitive.firstIndex = firstIndex;
				primitive.indexCount = indexCount;
				primitive.materialIndex = glTFPrimitive.material;
				node->mesh.primitives.push_back(primitive);
			}
		}

		if (parent) {
			parent->children.push_back(node);
		}
		else {
			nodes.push_back(node);
		}

		if (!node->mesh.primitives.empty())
		{
			linearMeshNodes.push_back(node);
		}
	}

	//Elysia
	void prepareMeshUniformBuffers(vks::VulkanDevice* vkDevice)
	{

		for (auto& meshNode : linearMeshNodes)
		{
			if (!meshNode->mesh.primitives.empty())
			{
				// Vertex shader uniform buffer block for Descriptor set 2
				VK_CHECK_RESULT(vkDevice->createBuffer(
					VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
					VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					&meshNode->mesh.uniformBuffer.buffer,
					sizeof(glm::mat4)));

				// Map persistent
				VK_CHECK_RESULT(meshNode->mesh.uniformBuffer.buffer.map());
			}
		}
	}

	//Elysia
	void updateMeshUniformBuffers()
	{
		for (auto& meshNode : linearMeshNodes)
		{
			if (!meshNode->mesh.primitives.empty() && meshNode->mesh.uniformBuffer.buffer.mapped)
			{
				glm::mat4 nodeMatrix = getNodeMatrix(meshNode);
				memcpy(meshNode->mesh.uniformBuffer.buffer.mapped, static_cast<const void*>(&nodeMatrix), sizeof(glm::mat4));
			}
		}
	}
	
	//Elysia
	VulkanglTFModel::Node* VulkanglTFModel::findNode(Node* parent, uint32_t index)
	{
		Node* nodeFound = nullptr;
		if (parent->index == index)
		{
			return parent;
		}
		for (auto& child : parent->children)
		{
			nodeFound = findNode(child, index);
			if (nodeFound)
			{
				break;
			}
		}
		return nodeFound;
	}
	//Elysia
	VulkanglTFModel::Node* VulkanglTFModel::nodeFromIndex(uint32_t index)
	{
		Node* nodeFound = nullptr;
		for (auto& node : nodes)
		{
			nodeFound = findNode(node, index);
			if (nodeFound)
			{
				break;
			}
		}
		return nodeFound;
	}
	

	//Elysia
	void VulkanglTFModel::loadAnimations(tinygltf::Model& input)
	{
		animations.resize(input.animations.size());

		for (size_t i = 0; i < input.animations.size(); i++)
		{
			tinygltf::Animation glTFAnimation = input.animations[i];
			animations[i].name = glTFAnimation.name;

			// Samplers
			animations[i].samplers.resize(glTFAnimation.samplers.size());
			for (size_t j = 0; j < glTFAnimation.samplers.size(); j++)
			{
				tinygltf::AnimationSampler glTFSampler = glTFAnimation.samplers[j];
				AnimationSampler& dstSampler = animations[i].samplers[j];
				dstSampler.interpolation = glTFSampler.interpolation;

				// Read sampler keyframe input time values
				{
					const tinygltf::Accessor& accessor = input.accessors[glTFSampler.input];
					const tinygltf::BufferView& bufferView = input.bufferViews[accessor.bufferView];
					const tinygltf::Buffer& buffer = input.buffers[bufferView.buffer];
					const void* dataPtr = &buffer.data[accessor.byteOffset + bufferView.byteOffset];
					const float* buf = static_cast<const float*>(dataPtr);
					for (size_t index = 0; index < accessor.count; index++)
					{
						dstSampler.inputs.push_back(buf[index]);
					}
					// Adjust animation's start and end times
					for (auto input : animations[i].samplers[j].inputs)
					{
						if (input < animations[i].start)
						{
							animations[i].start = input;
						};
						if (input > animations[i].end)
						{
							animations[i].end = input;
						}
					}
				}

				// Read sampler keyframe output translate/rotate/scale values
				{
					const tinygltf::Accessor& accessor = input.accessors[glTFSampler.output];
					const tinygltf::BufferView& bufferView = input.bufferViews[accessor.bufferView];
					const tinygltf::Buffer& buffer = input.buffers[bufferView.buffer];
					const void* dataPtr = &buffer.data[accessor.byteOffset + bufferView.byteOffset];
					switch (accessor.type)
					{
					case TINYGLTF_TYPE_VEC3: {
						const glm::vec3* buf = static_cast<const glm::vec3*>(dataPtr);
						for (size_t index = 0; index < accessor.count; index++)
						{
							dstSampler.outputsVec4.push_back(glm::vec4(buf[index], 0.0f));
						}
						break;
					}
					case TINYGLTF_TYPE_VEC4: {
						const glm::vec4* buf = static_cast<const glm::vec4*>(dataPtr);
						for (size_t index = 0; index < accessor.count; index++)
						{
							dstSampler.outputsVec4.push_back(buf[index]);
						}
						break;
					}
					default: {
						std::cout << "unknown type" << std::endl;
						break;
					}
					}
				}
			}

			// Channels
			animations[i].channels.resize(glTFAnimation.channels.size());
			for (size_t j = 0; j < glTFAnimation.channels.size(); j++)
			{
				tinygltf::AnimationChannel glTFChannel = glTFAnimation.channels[j];
				AnimationChannel& dstChannel = animations[i].channels[j];
				dstChannel.path = glTFChannel.target_path;
				dstChannel.samplerIndex = glTFChannel.sampler;
				dstChannel.node = nodeFromIndex(glTFChannel.target_node);
			}
		}
	}

	/*
		glTF rendering functions
	*/

	// Draw a single node including child nodes (if present)
	void drawNode(VkCommandBuffer commandBuffer, VkPipelineLayout pipelineLayout, VulkanglTFModel::Node* node)
	{
		vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 2, 1, &node->mesh.uniformBuffer.descriptorSet, 0, nullptr);

		if (!node->mesh.primitives.empty()) {
			//// Pass the node's matrix via push constants
			//// Traverse the node hierarchy to the top-most parent to get the final matrix of the current node
			//glm::mat4 nodeMatrix = getNodeMatrix(node);
			//// Pass the final matrix to the vertex shader using push constants
			//vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &nodeMatrix);
			for (VulkanglTFModel::Primitive& primitive : node->mesh.primitives) {
				if (primitive.indexCount > 0) {
					// Get the texture index for this primitive
					VulkanglTFModel::Texture texture = textures[materials[primitive.materialIndex].baseColorTextureIndex];
					// Bind the descriptor for the current primitive's texture
					vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 1, 1, &images[texture.imageIndex].descriptorSet, 0, nullptr);
					vkCmdDrawIndexed(commandBuffer, primitive.indexCount, 1, primitive.firstIndex, 0, 0);
				}
			}
		}
	}

	// Draw the glTF scene starting at the top-level-nodes
	void draw(VkCommandBuffer commandBuffer, VkPipelineLayout pipelineLayout)
	{
		// All vertices and indices are stored in single buffers, so we only need to bind once
		VkDeviceSize offsets[1] = { 0 };
		vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertices.buffer, offsets);
		vkCmdBindIndexBuffer(commandBuffer, indices.buffer, 0, VK_INDEX_TYPE_UINT32);
		// Render all nodes at top-level
		for (auto& node : linearMeshNodes) {
			drawNode(commandBuffer, pipelineLayout, node);
		}
	}
	//Elysia
	glm::mat4 VulkanglTFModel::getNodeMatrix(VulkanglTFModel::Node* node)
		{
			glm::mat4              nodeMatrix = node->getLocalMatrix();
			VulkanglTFModel::Node* currentParent = node->parent;
			while (currentParent)
			{
				nodeMatrix = currentParent->getLocalMatrix() * nodeMatrix;
				currentParent = currentParent->parent;
			}
			return nodeMatrix;
		}

	//Elysia
	void VulkanglTFModel::updateAnimation(float deltaTime)
		{
			if (activeAnimation > static_cast<uint32_t>(animations.size()) - 1)
			{
				std::cout << "No animation with index " << activeAnimation << std::endl;
				return;
			}
			Animation& animation = animations[activeAnimation];
			animation.currentTime += deltaTime;
			if (animation.currentTime > animation.end)
			{
				animation.currentTime -= animation.end;
			}

			for (auto& channel : animation.channels)
			{
				AnimationSampler& sampler = animation.samplers[channel.samplerIndex];
				for (size_t i = 0; i < sampler.inputs.size() - 1; i++)
				{
					if (sampler.interpolation != "LINEAR")
					{
						std::cout << "This sample only supports linear interpolations\n";
						continue;
					}

					// Get the input keyframe values for the current time stamp
					if ((animation.currentTime >= sampler.inputs[i]) && (animation.currentTime <= sampler.inputs[i + 1]))
					{
						float a = (animation.currentTime - sampler.inputs[i]) / (sampler.inputs[i + 1] - sampler.inputs[i]);
						if (channel.path == "translation")
						{
							channel.node->translation = glm::mix(sampler.outputsVec4[i], sampler.outputsVec4[i + 1], a);
						}
						if (channel.path == "rotation")
						{
							glm::quat q1;
							q1.x = sampler.outputsVec4[i].x;
							q1.y = sampler.outputsVec4[i].y;
							q1.z = sampler.outputsVec4[i].z;
							q1.w = sampler.outputsVec4[i].w;

							glm::quat q2;
							q2.x = sampler.outputsVec4[i + 1].x;
							q2.y = sampler.outputsVec4[i + 1].y;
							q2.z = sampler.outputsVec4[i + 1].z;
							q2.w = sampler.outputsVec4[i + 1].w;

							channel.node->rotation = glm::normalize(glm::slerp(q1, q2, a));
						}
						if (channel.path == "scale")
						{
							channel.node->scale = glm::mix(sampler.outputsVec4[i], sampler.outputsVec4[i + 1], a);
						}
					}
				}
			}
			updateMeshUniformBuffers();
		}
};

class VulkanExample : public VulkanExampleBase
{
public:
	bool wireframe = false;
	bool test1 = false;

	VulkanglTFModel glTFModel;
	
	struct ShaderData {
		vks::Buffer buffer;
		struct Values {
			glm::mat4 projection;
			glm::mat4 model;
			glm::vec4 lightPos = glm::vec4(5.0f, 5.0f, -5.0f, 1.0f);
			glm::vec4 viewPos;
		} values;
	} shaderData;

	struct Pipelines {
		VkPipeline solid;
		VkPipeline wireframe = VK_NULL_HANDLE;
	} pipelines;

	VkPipelineLayout pipelineLayout;
	VkDescriptorSet descriptorSet;

	struct DescriptorSetLayouts {
		//set 0
		VkDescriptorSetLayout matrices;
		//set 1
		VkDescriptorSetLayout textures;
		//set 2
		VkDescriptorSetLayout nodes;
	} descriptorSetLayouts;

	VulkanExample() : VulkanExampleBase(ENABLE_VALIDATION)
	{
		title = "homework1";
		camera.type = Camera::CameraType::lookat;
		camera.flipY = true;
		camera.setPosition(glm::vec3(0.0f, -0.1f, -1.0f));
		camera.setRotation(glm::vec3(0.0f, 45.0f, 0.0f));
		camera.setPerspective(60.0f, (float)width / (float)height, 0.1f, 256.0f);
		//timerSpeed = 1.0f;
		//paused = true;
	}

	~VulkanExample()
	{
		// Clean up used Vulkan resources
		// Note : Inherited destructor cleans up resources stored in base class
		vkDestroyPipeline(device, pipelines.solid, nullptr);
		if (pipelines.wireframe != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, pipelines.wireframe, nullptr);
		}

		vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.matrices, nullptr);
		vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.textures, nullptr);

		shaderData.buffer.destroy();
	}

	virtual void getEnabledFeatures()
	{
		// Fill mode non solid is required for wireframe display
		if (deviceFeatures.fillModeNonSolid) {
			enabledFeatures.fillModeNonSolid = VK_TRUE;
		};
	}

	void buildCommandBuffers()
	{
		VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();

		VkClearValue clearValues[2];
		clearValues[0].color = defaultClearColor;
		clearValues[0].color = { { 0.25f, 0.25f, 0.25f, 1.0f } };;
		clearValues[1].depthStencil = { 1.0f, 0 };

		VkRenderPassBeginInfo renderPassBeginInfo = vks::initializers::renderPassBeginInfo();
		renderPassBeginInfo.renderPass = renderPass;
		renderPassBeginInfo.renderArea.offset.x = 0;
		renderPassBeginInfo.renderArea.offset.y = 0;
		renderPassBeginInfo.renderArea.extent.width = width;
		renderPassBeginInfo.renderArea.extent.height = height;
		renderPassBeginInfo.clearValueCount = 2;
		renderPassBeginInfo.pClearValues = clearValues;

		const VkViewport viewport = vks::initializers::viewport((float)width, (float)height, 0.0f, 1.0f);
		const VkRect2D scissor = vks::initializers::rect2D(width, height, 0, 0);

		for (int32_t i = 0; i < drawCmdBuffers.size(); ++i)
		{
			renderPassBeginInfo.framebuffer = frameBuffers[i];
			VK_CHECK_RESULT(vkBeginCommandBuffer(drawCmdBuffers[i], &cmdBufInfo));
			vkCmdBeginRenderPass(drawCmdBuffers[i], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);
			vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);
			// Bind scene matrices descriptor to set 0
			vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
			vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, wireframe ? pipelines.wireframe : pipelines.solid);
			
			glTFModel.draw(drawCmdBuffers[i], pipelineLayout);
			
			drawUI(drawCmdBuffers[i]);
			vkCmdEndRenderPass(drawCmdBuffers[i]);
			VK_CHECK_RESULT(vkEndCommandBuffer(drawCmdBuffers[i]));
		}
	}

	void loadglTFFile(std::string filename)
	{
		tinygltf::Model glTFInput;
		tinygltf::TinyGLTF gltfContext;
		std::string error, warning;

		//多余？
		this->device = device;

#if defined(__ANDROID__)
		// On Android all assets are packed with the apk in a compressed form, so we need to open them using the asset manager
		// We let tinygltf handle this, by passing the asset manager of our app
		tinygltf::asset_manager = androidApp->activity->assetManager;
#endif
		bool fileLoaded = gltfContext.LoadASCIIFromFile(&glTFInput, &error, &warning, filename);

		// Pass some Vulkan resources required for setup and rendering to the glTF model loading class
		glTFModel.vulkanDevice = vulkanDevice;
		glTFModel.copyQueue = queue;
		
		std::vector<uint32_t> indexBuffer;
		std::vector<VulkanglTFModel::Vertex> vertexBuffer;

		if (fileLoaded) {
			glTFModel.loadImages(glTFInput);
			glTFModel.loadMaterials(glTFInput);
			glTFModel.loadTextures(glTFInput);
			const tinygltf::Scene& scene = glTFInput.scenes[0];
			for (size_t i = 0; i < scene.nodes.size(); i++) {
				const tinygltf::Node node = glTFInput.nodes[scene.nodes[i]];
				glTFModel.loadNode(node, glTFInput, nullptr, scene.nodes[i],indexBuffer, vertexBuffer);

			}
			//Elysia
			glTFModel.loadAnimations(glTFInput);
		}
		else {
			vks::tools::exitFatal("Could not open the glTF file.\n\nThe file is part of the additional asset pack.\n\nRun \"download_assets.py\" in the repository root to download the latest version.", -1);
			return;
		}
		
		// Create and upload vertex and index buffer
		// We will be using one single vertex buffer and one single index buffer for the whole glTF scene
		// Primitives (of the glTF model) will then index into these using index offsets
		
		size_t vertexBufferSize = vertexBuffer.size() * sizeof(VulkanglTFModel::Vertex);
		size_t indexBufferSize = indexBuffer.size() * sizeof(uint32_t);
		glTFModel.indices.count = static_cast<uint32_t>(indexBuffer.size());

		struct StagingBuffer {
			VkBuffer buffer;
			VkDeviceMemory memory;
		} vertexStaging, indexStaging;

		// Create host visible staging buffers (source)
		//创建主机可见的临时缓冲区（源）
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			vertexBufferSize,
			&vertexStaging.buffer,
			&vertexStaging.memory,
			vertexBuffer.data()));
		// Index data
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			indexBufferSize,
			&indexStaging.buffer,
			&indexStaging.memory,
			indexBuffer.data()));

		// Create device local buffers (target)
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			vertexBufferSize,
			&glTFModel.vertices.buffer,
			&glTFModel.vertices.memory));
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			indexBufferSize,
			&glTFModel.indices.buffer,
			&glTFModel.indices.memory));

		// Copy data from staging buffers (host) do device local buffer (gpu)
		VkCommandBuffer copyCmd = vulkanDevice->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
		VkBufferCopy copyRegion = {};

		copyRegion.size = vertexBufferSize;
		vkCmdCopyBuffer(
			copyCmd,
			vertexStaging.buffer,
			glTFModel.vertices.buffer,
			1,
			&copyRegion);

		copyRegion.size = indexBufferSize;
		vkCmdCopyBuffer(
			copyCmd,
			indexStaging.buffer,
			glTFModel.indices.buffer,
			1,
			&copyRegion);

		vulkanDevice->flushCommandBuffer(copyCmd, queue, true);

		// Free staging resources
		vkDestroyBuffer(device, vertexStaging.buffer, nullptr);
		vkFreeMemory(device, vertexStaging.memory, nullptr);
		vkDestroyBuffer(device, indexStaging.buffer, nullptr);
		vkFreeMemory(device, indexStaging.memory, nullptr);
	}

	void loadAssets()
	{
		loadglTFFile(getAssetPath() + "buster_drone/busterDrone.gltf");
	}

	void setupDescriptors()
	{
		/*
		This sample uses separate descriptor sets (and layouts) for the matrices and materials (textures)
	*/

		std::vector<VkDescriptorPoolSize> poolSizes = {
			vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 + static_cast<uint32_t>(glTFModel.linearMeshNodes.size())),
			// One combined image sampler per model image/texture
			vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<uint32_t>(glTFModel.images.size())),
		};
		// One set for matrices and one per model image/texture
		const uint32_t maxSetCount = static_cast<uint32_t>(glTFModel.images.size()) + static_cast<uint32_t>(glTFModel.linearMeshNodes.size()) + 1;
		VkDescriptorPoolCreateInfo descriptorPoolInfo = vks::initializers::descriptorPoolCreateInfo(poolSizes, maxSetCount);
		VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool));

		{
			// Descriptor set 0 layout : passing matrices
			VkDescriptorSetLayoutBinding setLayoutBinding = vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT, 0);
			VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCI = vks::initializers::descriptorSetLayoutCreateInfo(&setLayoutBinding, 1);
			VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorSetLayoutCI, nullptr, &descriptorSetLayouts.matrices));

			// Setup Descriptor Set for scene matrices
			VkDescriptorSetAllocateInfo allocInfo = vks::initializers::descriptorSetAllocateInfo(descriptorPool, &descriptorSetLayouts.matrices, 1);
			VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet));
			VkWriteDescriptorSet writeDescriptorSet = vks::initializers::writeDescriptorSet(descriptorSet, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &shaderData.buffer.descriptor);
			vkUpdateDescriptorSets(device, 1, &writeDescriptorSet, 0, nullptr);
		}

		{
			// Descriptor set 1 layout : passing material textures
			VkDescriptorSetLayoutBinding setLayoutBinding = vks::initializers::descriptorSetLayoutBinding(
				VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 0);
			VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCI = vks::initializers::descriptorSetLayoutCreateInfo(&setLayoutBinding, 1);
			VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorSetLayoutCI, nullptr, &descriptorSetLayouts.textures));

			// Descriptor Sets for materials
			for (auto& image : glTFModel.images) {
				const VkDescriptorSetAllocateInfo allocInfo = vks::initializers::descriptorSetAllocateInfo(descriptorPool, &descriptorSetLayouts.textures, 1);
				VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &image.descriptorSet))
					VkWriteDescriptorSet writeDescriptorSet = vks::initializers::writeDescriptorSet(image.descriptorSet, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0, &image.texture.descriptor);
				vkUpdateDescriptorSets(device, 1, &writeDescriptorSet, 0, nullptr);
			}
		}

		{
			// Descriptor set 2 layout : passing glTF Node data
			VkDescriptorSetLayoutBinding setLayoutBinding = vks::initializers::descriptorSetLayoutBinding(
				VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT, 0);
			VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCI = vks::initializers::descriptorSetLayoutCreateInfo(&setLayoutBinding, 1);
			VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorSetLayoutCI, nullptr, &descriptorSetLayouts.nodes))

				// Descriptor Sets for glTF Node data
				for (auto& meshNode : glTFModel.linearMeshNodes)
				{
					if (!meshNode->mesh.primitives.empty())
					{
						const VkDescriptorSetAllocateInfo allocInfo = vks::initializers::descriptorSetAllocateInfo(descriptorPool, &descriptorSetLayouts.nodes, 1);
						VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &meshNode->mesh.uniformBuffer.descriptorSet));
						//VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &node->mesh.uniformBuffer.descriptorSet));
						VkWriteDescriptorSet writeDescriptorSet = vks::initializers::writeDescriptorSet(
							meshNode->mesh.uniformBuffer.descriptorSet, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0,
							&meshNode->mesh.uniformBuffer.buffer.descriptor);
						vkUpdateDescriptorSets(device, 1, &writeDescriptorSet, 0, nullptr);
					}
				}
		}

		// Pipeline Layout using both descriptor sets (set 0 = matrices, set 1 = material, set2 = node)
		std::array<VkDescriptorSetLayout, 3> setLayouts = { descriptorSetLayouts.matrices, descriptorSetLayouts.textures, descriptorSetLayouts.nodes };
		VkPipelineLayoutCreateInfo pipelineLayoutCI = vks::initializers::pipelineLayoutCreateInfo(
			setLayouts.data(), static_cast<uint32_t>(setLayouts.size()));

		//// Push constants to push the local matrices of a primitive to the vertex shader
		//VkPushConstantRange pushConstantRange = vks::initializers::pushConstantRange(VK_SHADER_STAGE_VERTEX_BIT, sizeof(glm::mat4), 0);
		//// Push constant ranges are part of the pipeline layout
		//pipelineLayoutCI.pushConstantRangeCount = 1;
		//pipelineLayoutCI.pPushConstantRanges = &pushConstantRange;

		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCI, nullptr, &pipelineLayout));
	}

	void preparePipelines()
	{
		VkPipelineInputAssemblyStateCreateInfo inputAssemblyStateCI = vks::initializers::pipelineInputAssemblyStateCreateInfo(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);
		VkPipelineRasterizationStateCreateInfo rasterizationStateCI = vks::initializers::pipelineRasterizationStateCreateInfo(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0);
		VkPipelineColorBlendAttachmentState blendAttachmentStateCI = vks::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE);
		VkPipelineColorBlendStateCreateInfo colorBlendStateCI = vks::initializers::pipelineColorBlendStateCreateInfo(1, &blendAttachmentStateCI);
		VkPipelineDepthStencilStateCreateInfo depthStencilStateCI = vks::initializers::pipelineDepthStencilStateCreateInfo(VK_TRUE, VK_TRUE, VK_COMPARE_OP_LESS_OR_EQUAL);
		VkPipelineViewportStateCreateInfo viewportStateCI = vks::initializers::pipelineViewportStateCreateInfo(1, 1, 0);
		VkPipelineMultisampleStateCreateInfo multisampleStateCI = vks::initializers::pipelineMultisampleStateCreateInfo(VK_SAMPLE_COUNT_1_BIT, 0);
		const std::vector<VkDynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicStateCI = vks::initializers::pipelineDynamicStateCreateInfo(dynamicStateEnables.data(), static_cast<uint32_t>(dynamicStateEnables.size()), 0);
		// Vertex input bindings and attributes
		const std::vector<VkVertexInputBindingDescription> vertexInputBindings = {
			vks::initializers::vertexInputBindingDescription(0, sizeof(VulkanglTFModel::Vertex), VK_VERTEX_INPUT_RATE_VERTEX),
		};
		const std::vector<VkVertexInputAttributeDescription> vertexInputAttributes = {
			vks::initializers::vertexInputAttributeDescription(0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(VulkanglTFModel::Vertex, pos)),	// Location 0: Position
			vks::initializers::vertexInputAttributeDescription(0, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(VulkanglTFModel::Vertex, normal)),// Location 1: Normal
			vks::initializers::vertexInputAttributeDescription(0, 2, VK_FORMAT_R32G32B32_SFLOAT, offsetof(VulkanglTFModel::Vertex, uv)),	// Location 2: Texture coordinates
			vks::initializers::vertexInputAttributeDescription(0, 3, VK_FORMAT_R32G32B32_SFLOAT, offsetof(VulkanglTFModel::Vertex, color)),	// Location 3: Color
		};
		VkPipelineVertexInputStateCreateInfo vertexInputStateCI = vks::initializers::pipelineVertexInputStateCreateInfo();
		vertexInputStateCI.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexInputBindings.size());
		vertexInputStateCI.pVertexBindingDescriptions = vertexInputBindings.data();
		vertexInputStateCI.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexInputAttributes.size());
		vertexInputStateCI.pVertexAttributeDescriptions = vertexInputAttributes.data();

		const std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = {
			loadShader(getHomeworkShadersPath() + "homework1/mesh.vert.spv", VK_SHADER_STAGE_VERTEX_BIT),
			loadShader(getHomeworkShadersPath() + "homework1/mesh.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT)
		};

		VkGraphicsPipelineCreateInfo pipelineCI = vks::initializers::pipelineCreateInfo(pipelineLayout, renderPass, 0);
		pipelineCI.pVertexInputState = &vertexInputStateCI;
		pipelineCI.pInputAssemblyState = &inputAssemblyStateCI;
		pipelineCI.pRasterizationState = &rasterizationStateCI;
		pipelineCI.pColorBlendState = &colorBlendStateCI;
		pipelineCI.pMultisampleState = &multisampleStateCI;
		pipelineCI.pViewportState = &viewportStateCI;
		pipelineCI.pDepthStencilState = &depthStencilStateCI;
		pipelineCI.pDynamicState = &dynamicStateCI;
		pipelineCI.stageCount = static_cast<uint32_t>(shaderStages.size());
		pipelineCI.pStages = shaderStages.data();

		// Solid rendering pipeline
		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.solid));

		// Wire frame rendering pipeline
		if (deviceFeatures.fillModeNonSolid) {
			rasterizationStateCI.polygonMode = VK_POLYGON_MODE_LINE;
			rasterizationStateCI.lineWidth = 1.0f;
			VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.wireframe));
		}
	}

	// Prepare and initialize uniform buffer containing shader uniforms
	void prepareUniformBuffers()
	{
		// Vertex shader uniform buffer block
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			&shaderData.buffer,
			sizeof(shaderData.values)));

		// Map persistent
		VK_CHECK_RESULT(shaderData.buffer.map());

		//Elysia
		glTFModel.prepareMeshUniformBuffers(vulkanDevice);

		updateUniformBuffers();
	}
	
	void updateUniformBuffers()
	{
		shaderData.values.projection = camera.matrices.perspective;
		shaderData.values.model = camera.matrices.view;
		shaderData.values.viewPos = camera.viewPos;
		memcpy(shaderData.buffer.mapped, &shaderData.values, sizeof(shaderData.values));
		//Elysia
		//glTFModel.updateMeshUniformBuffers();
	}
	
	void prepare()
	{
		VulkanExampleBase::prepare();
		loadAssets();
		prepareUniformBuffers();
		setupDescriptors();
		preparePipelines();
		buildCommandBuffers();
		prepared = true;
	}

	//Elysia
	void updateLights()
	{
		shaderData.values.lightPos.x = sin(glm::radians(timer * 360.0f)) * 20.0f;
		shaderData.values.lightPos.z = cos(glm::radians(timer * 360.0f)) * 20.0f;
	}

	virtual void render()
	{
		renderFrame();
		if (camera.updated) {
			updateUniformBuffers();
		}
		//Elysia
		if (!paused)
		{
			updateLights();
			glTFModel.updateAnimation(frameTimer); 
		}
	}

	virtual void viewChanged()
	{
		updateUniformBuffers();
	}

	virtual void OnUpdateUIOverlay(vks::UIOverlay* overlay)
	{
		//Elysia
		if (overlay->header("Settings")) 
		{
			if (overlay->checkBox("Wireframe", &wireframe)) 
			{
				buildCommandBuffers();
			}
			//overlay->checkBox("test1", &test1);
		}
	}
};


VulkanExample* vulkanExample; 

LRESULT __stdcall WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) 
{
	if (vulkanExample != 0) 
	{
		vulkanExample->handleMessages(hWnd, uMsg, wParam, lParam);
	} 
	return (DefWindowProcA(hWnd, uMsg, wParam, lParam));
} 

int __stdcall WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) 
{
	for (int32_t i = 0; i < (*__p___argc()); i++) 
	{
		VulkanExample::args.push_back((*__p___argv())[i]);
	}; 
	vulkanExample = new VulkanExample(); 
	vulkanExample->initVulkan(); 
	vulkanExample->setupWindow(hInstance, WndProc); 
	vulkanExample->prepare(); 
	vulkanExample->renderLoop(); 
	delete(vulkanExample); 
	return 0;
}
