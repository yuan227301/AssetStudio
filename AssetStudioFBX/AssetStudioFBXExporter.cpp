#include <fbxsdk.h>
#include <fbxsdk/fileio/fbxiosettings.h>
#include "AssetStudioFBX.h"

namespace AssetStudio
{
	void Fbx::Exporter::Export(String^ path, IImported^ imported, bool eulerFilter, float filterPrecision, bool allFrames, bool allBones, bool skins, float boneSize, float scaleFactor, bool flatInbetween, int versionIndex, bool isAscii)
	{
		FileInfo^ file = gcnew FileInfo(path);
		DirectoryInfo^ dir = file->Directory;
		if (!dir->Exists)
		{
			dir->Create();
		}
		String^ currentDir = Directory::GetCurrentDirectory();
		Directory::SetCurrentDirectory(dir->FullName);
		path = Path::GetFileName(path);

		Exporter^ exporter = gcnew Exporter(path, imported, allFrames, allBones, skins, boneSize, scaleFactor, versionIndex, isAscii, true);
		exporter->ExportMorphs(imported, false, flatInbetween);
		exporter->ExportAnimations(eulerFilter, filterPrecision, flatInbetween);
		exporter->pExporter->Export(exporter->pScene);
		delete exporter;

		Directory::SetCurrentDirectory(currentDir);
	}

	void Fbx::Exporter::ExportMorph(String^ path, IImported^ imported, bool morphMask, bool flatInbetween, bool skins, float boneSize, float scaleFactor, int versionIndex, bool isAscii)
	{
		FileInfo^ file = gcnew FileInfo(path);
		DirectoryInfo^ dir = file->Directory;
		if (!dir->Exists)
		{
			dir->Create();
		}
		String^ currentDir = Directory::GetCurrentDirectory();
		Directory::SetCurrentDirectory(dir->FullName);
		path = Path::GetFileName(path);

		Exporter^ exporter = gcnew Exporter(path, imported, false, true, skins, boneSize, scaleFactor, versionIndex, isAscii, false);
		exporter->ExportMorphs(imported, morphMask, flatInbetween);
		exporter->pExporter->Export(exporter->pScene);
		delete exporter;

		Directory::SetCurrentDirectory(currentDir);
	}

	Fbx::Exporter::Exporter(String^ path, IImported^ imported, bool allFrames, bool allBones, bool skins, float boneSize, float scaleFactor, int versionIndex, bool isAscii, bool normals)
	{
		this->imported = imported;
		exportSkins = skins;
		this->boneSize = boneSize;

		cDest = NULL;
		pSdkManager = NULL;
		pScene = NULL;
		pExporter = NULL;
		pMaterials = NULL;
		pTextures = NULL;
		pMeshNodes = NULL;

		pin_ptr<FbxManager*> pSdkManagerPin = &pSdkManager;
		pin_ptr<FbxScene*> pScenePin = &pScene;
		Init(pSdkManagerPin, pScenePin);

		IOS_REF.SetBoolProp(EXP_FBX_MATERIAL, true);
		IOS_REF.SetBoolProp(EXP_FBX_TEXTURE, true);
		IOS_REF.SetBoolProp(EXP_FBX_EMBEDDED, false);
		IOS_REF.SetBoolProp(EXP_FBX_SHAPE, true);
		IOS_REF.SetBoolProp(EXP_FBX_GOBO, true);
		IOS_REF.SetBoolProp(EXP_FBX_ANIMATION, true);
		IOS_REF.SetBoolProp(EXP_FBX_GLOBAL_SETTINGS, true);

		FbxGlobalSettings& globalSettings = pScene->GetGlobalSettings();
		globalSettings.SetSystemUnit(FbxSystemUnit(scaleFactor));

		cDest = StringToCharArray(path);
		pExporter = FbxExporter::Create(pScene, "");

		int pFileFormat = 0;
		if (versionIndex == 0)
		{
			pFileFormat = 3;
			if (isAscii)
			{
				pFileFormat = 4;
			}
		}
		else
		{
			pExporter->SetFileExportVersion(FBXVersion[versionIndex]);
			if (isAscii)
			{
				pFileFormat = 1;
			}
		}

		if (!pExporter->Initialize(cDest, pFileFormat, pSdkManager->GetIOSettings()))
		{
			throw gcnew Exception(gcnew String("Failed to initialize FbxExporter: ") + gcnew String(pExporter->GetStatus().GetErrorString()));
		}

		frameNames = nullptr;
		if (!allFrames)
		{
			frameNames = SearchHierarchy();
			if (!frameNames)
			{
				return;
			}
		}

		pMeshNodes = imported->MeshList != nullptr ? new FbxArray<FbxNode*>(imported->MeshList->Count) : NULL;
		ExportFrame(pScene->GetRootNode(), imported->RootFrame);

		if (imported->MeshList != nullptr)
		{
			SetJointsFromImportedMeshes(allBones);

			pMaterials = new FbxArray<FbxSurfacePhong*>();
			pTextures = new FbxArray<FbxFileTexture*>();
			pMaterials->Reserve(imported->MaterialList->Count);
			pTextures->Reserve(imported->TextureList->Count);

			for (int i = 0; i < pMeshNodes->GetCount(); i++)
			{
				FbxNode* meshNode = pMeshNodes->GetAt(i);
				String^ meshPath = gcnew String(meshNode->GetName());
				FbxNode* rootNode = meshNode;
				while ((rootNode = rootNode->GetParent()) != pScene->GetRootNode())
				{
					meshPath = gcnew String(rootNode->GetName()) + "/" + meshPath;
				}
				ImportedMesh^ mesh = ImportedHelpers::FindMesh(meshPath, imported->MeshList);
				ExportMesh(meshNode, mesh, normals);
			}
		}
		else
		{
			SetJointsNode(pScene->GetRootNode()->GetChild(0), nullptr, true);
		}
	}

	Fbx::Exporter::~Exporter()
	{
		if (pMeshNodes != NULL)
		{
			delete pMeshNodes;
		}
		if (pMaterials != NULL)
		{
			delete pMaterials;
		}
		if (pTextures != NULL)
		{
			delete pTextures;
		}
		if (pExporter != NULL)
		{
			pExporter->Destroy();
		}
		if (pScene != NULL)
		{
			pScene->Destroy();
		}
		if (pSdkManager != NULL)
		{
			pSdkManager->Destroy();
		}
		if (cDest != NULL)
		{
			Marshal::FreeHGlobal((IntPtr)cDest);
		}
	}

	void Fbx::Exporter::SetJointsNode(FbxNode* pNode, HashSet<String^>^ boneNames, bool allBones)
	{
		String^ nodeName = gcnew String(pNode->GetName());
		if (allBones || boneNames->Contains(nodeName))
		{
			FbxSkeleton* pJoint = FbxSkeleton::Create(pSdkManager, "");
			pJoint->Size.Set((double)boneSize);
			pJoint->SetSkeletonType(FbxSkeleton::eLimbNode);
			pNode->SetNodeAttribute(pJoint);
		}
		else
		{
			FbxNull* pNull = FbxNull::Create(pSdkManager, "");
			if (pNode->GetChildCount() > 0)
			{
				pNull->Look.Set(FbxNull::eNone);
			}

			pNode->SetNodeAttribute(pNull);
		}

		for (int i = 0; i < pNode->GetChildCount(); i++)
		{
			SetJointsNode(pNode->GetChild(i), boneNames, allBones);
		}
	}

	HashSet<String^>^ Fbx::Exporter::SearchHierarchy()
	{
		if (imported->MeshList == nullptr || imported->MeshList->Count == 0)
		{
			return nullptr;
		}
		HashSet<String^>^ exportFrames = gcnew HashSet<String^>();
		SearchHierarchy(imported->RootFrame, exportFrames);
		return exportFrames;
	}

	void Fbx::Exporter::SearchHierarchy(ImportedFrame^ frame, HashSet<String^>^ exportFrames)
	{
		ImportedMesh^ meshListSome = ImportedHelpers::FindMesh(frame, imported->MeshList);
		if (meshListSome != nullptr)
		{
			ImportedFrame^ parent = frame;
			while (parent != nullptr)
			{
				exportFrames->Add(parent->Name);
				parent = parent->Parent;
			}

			List<ImportedBone^>^ boneList = meshListSome->BoneList;
			if (boneList != nullptr)
			{
				for (int i = 0; i < boneList->Count; i++)
				{
					String^ boneName = boneList[i]->Path->Substring(boneList[i]->Path->LastIndexOf('/') + 1);
					if (!exportFrames->Contains(boneName))
					{
						ImportedFrame^ boneParent = imported->RootFrame->FindFrameByPath(boneList[i]->Path);
						while (boneParent != nullptr)
						{
							exportFrames->Add(boneParent->Name);
							boneParent = boneParent->Parent;
						}
					}
				}
			}
		}

		for (int i = 0; i < frame->Count; i++)
		{
			SearchHierarchy(frame[i], exportFrames);
		}
	}

	void Fbx::Exporter::SetJointsFromImportedMeshes(bool allBones)
	{
		if (!exportSkins)
		{
			return;
		}
		HashSet<String^>^ boneNames = gcnew HashSet<String^>();
		for (int i = 0; i < imported->MeshList->Count; i++)
		{
			ImportedMesh^ meshList = imported->MeshList[i];
			List<ImportedBone^>^ boneList = meshList->BoneList;
			if (boneList != nullptr)
			{
				for (int j = 0; j < boneList->Count; j++)
				{
					ImportedBone^ bone = boneList[j];
					boneNames->Add(bone->Path);
				}
			}
		}

		SetJointsNode(pScene->GetRootNode()->GetChild(0), boneNames, allBones);
	}

	void Fbx::Exporter::ExportFrame(FbxNode* pParentNode, ImportedFrame^ frame)
	{
		String^ frameName = frame->Name;
		if ((frameNames == nullptr) || frameNames->Contains(frameName))
		{
			FbxNode* pFrameNode = NULL;
			char* pName = NULL;
			try
			{
				pName = StringToCharArray(frameName);
				pFrameNode = FbxNode::Create(pScene, pName);
			}
			finally
			{
				Marshal::FreeHGlobal((IntPtr)pName);
			}

			pFrameNode->LclScaling.Set(FbxDouble3(frame->LocalScale.X, frame->LocalScale.Y, frame->LocalScale.Z));
			pFrameNode->LclRotation.Set(FbxDouble3(frame->LocalRotation.X, frame->LocalRotation.Y, frame->LocalRotation.Z));
			pFrameNode->LclTranslation.Set(FbxDouble3(frame->LocalPosition.X, frame->LocalPosition.Y, frame->LocalPosition.Z));
			pParentNode->AddChild(pFrameNode);

			if (imported->MeshList != nullptr && ImportedHelpers::FindMesh(frame, imported->MeshList) != nullptr)
			{
				pMeshNodes->Add(pFrameNode);
			}

			for (int i = 0; i < frame->Count; i++)
			{
				ExportFrame(pFrameNode, frame[i]);
			}
		}
	}

	void Fbx::Exporter::ExportMesh(FbxNode* pFrameNode, ImportedMesh^ meshList, bool normals)
	{
		int lastSlash = meshList->Path->LastIndexOf('/');
		String^ frameName = lastSlash < 0 ? meshList->Path : meshList->Path->Substring(lastSlash + 1);
		List<ImportedBone^>^ boneList = meshList->BoneList;
		bool hasBones;
		if (exportSkins && boneList != nullptr)
		{
			hasBones = boneList->Count > 0;
		}
		else
		{
			hasBones = false;
		}

		FbxArray<FbxNode*>* pBoneNodeList = NULL;
		try
		{
			if (hasBones)
			{
				pBoneNodeList = new FbxArray<FbxNode*>();
				pBoneNodeList->Reserve(boneList->Count);
				for (int i = 0; i < boneList->Count; i++)
				{
					ImportedBone^ bone = boneList[i];
					FbxNode* lFrame = FindNodeByPath(bone->Path, false);
					pBoneNodeList->Add(lFrame);
				}
			}

			for (int i = 0; i < meshList->SubmeshList->Count; i++)
			{
				char* pName = NULL;
				FbxArray<FbxCluster*>* pClusterArray = NULL;
				try
				{
					pName = StringToCharArray(frameName + "_" + i);
					FbxMesh* pMesh = FbxMesh::Create(pScene, "");

					if (hasBones)
					{
						pClusterArray = new FbxArray<FbxCluster*>();
						pClusterArray->Reserve(boneList->Count);

						for (int i = 0; i < boneList->Count; i++)
						{
							FbxNode* pNode = pBoneNodeList->GetAt(i);
							FbxString lClusterName = pNode->GetNameOnly() + FbxString("Cluster");
							FbxCluster* pCluster = FbxCluster::Create(pSdkManager, lClusterName.Buffer());
							pCluster->SetLink(pNode);
							pCluster->SetLinkMode(FbxCluster::eTotalOne);
							pClusterArray->Add(pCluster);
						}
					}

					ImportedSubmesh^ meshObj = meshList->SubmeshList[i];
					List<ImportedFace^>^ faceList = meshObj->FaceList;
					List<ImportedVertex^>^ vertexList = meshObj->VertexList;

					pMesh->InitControlPoints(vertexList->Count);
					FbxVector4* pControlPoints = pMesh->GetControlPoints();

					FbxGeometryElementNormal* lGeometryElementNormal = NULL;
					//if (normals)
					{
						lGeometryElementNormal = pMesh->GetElementNormal();
						if (!lGeometryElementNormal)
						{
							lGeometryElementNormal = pMesh->CreateElementNormal();
						}
						lGeometryElementNormal->SetMappingMode(FbxGeometryElement::eByControlPoint);
						lGeometryElementNormal->SetReferenceMode(FbxGeometryElement::eDirect);
					}

					FbxGeometryElementUV* lGeometryElementUV = pMesh->GetElementUV();
					if (!lGeometryElementUV)
					{
						lGeometryElementUV = pMesh->CreateElementUV("");
					}
					lGeometryElementUV->SetMappingMode(FbxGeometryElement::eByControlPoint);
					lGeometryElementUV->SetReferenceMode(FbxGeometryElement::eDirect);

					FbxGeometryElementTangent* lGeometryElementTangent = NULL;
					if (normals)
					{
						lGeometryElementTangent = pMesh->GetElementTangent();
						if (!lGeometryElementTangent)
						{
							lGeometryElementTangent = pMesh->CreateElementTangent();
						}
						lGeometryElementTangent->SetMappingMode(FbxGeometryElement::eByControlPoint);
						lGeometryElementTangent->SetReferenceMode(FbxGeometryElement::eDirect);
					}

					bool vertexColours = vertexList->Count > 0 && dynamic_cast<ImportedVertexWithColour^>(vertexList[0]) != nullptr;
					if (vertexColours)
					{
						FbxGeometryElementVertexColor* lGeometryElementVertexColor = pMesh->CreateElementVertexColor();
						lGeometryElementVertexColor->SetMappingMode(FbxGeometryElement::eByControlPoint);
						lGeometryElementVertexColor->SetReferenceMode(FbxGeometryElement::eDirect);
						for (int j = 0; j < vertexList->Count; j++)
						{
							ImportedVertexWithColour^ vert = (ImportedVertexWithColour^)vertexList[j];
							lGeometryElementVertexColor->GetDirectArray().Add(FbxColor(vert->Colour.R, vert->Colour.G, vert->Colour.B, vert->Colour.A));
						}
					}

					FbxNode* pMeshNode = FbxNode::Create(pScene, pName);
					pMeshNode->SetNodeAttribute(pMesh);
					pFrameNode->AddChild(pMeshNode);

					ImportedMaterial^ mat = ImportedHelpers::FindMaterial(meshObj->Material, imported->MaterialList);
					if (mat != nullptr)
					{
						FbxGeometryElementMaterial* lGeometryElementMaterial = pMesh->GetElementMaterial();
						if (!lGeometryElementMaterial)
						{
							lGeometryElementMaterial = pMesh->CreateElementMaterial();
						}
						lGeometryElementMaterial->SetMappingMode(FbxGeometryElement::eByPolygon);
						lGeometryElementMaterial->SetReferenceMode(FbxGeometryElement::eIndexToDirect);

						char* pMatName = NULL;
						try
						{
							pMatName = StringToCharArray(mat->Name);
							int foundMat = -1;
							for (int j = 0; j < pMaterials->GetCount(); j++)
							{
								FbxSurfacePhong* pMatTemp = pMaterials->GetAt(j);
								if (strcmp(pMatTemp->GetName(), pMatName) == 0)
								{
									foundMat = j;
									break;
								}
							}

							FbxSurfacePhong* pMat;
							if (foundMat >= 0)
							{
								pMat = pMaterials->GetAt(foundMat);
							}
							else
							{
								FbxString lShadingName = "Phong";
								Color diffuse = mat->Diffuse;
								Color ambient = mat->Ambient;
								Color emissive = mat->Emissive;
								Color specular = mat->Specular;
								Color reflection = mat->Reflection;
								pMat = FbxSurfacePhong::Create(pScene, pMatName);
								pMat->Diffuse.Set(FbxDouble3(diffuse.R, diffuse.G, diffuse.B));
								pMat->DiffuseFactor.Set(FbxDouble(diffuse.A));
								pMat->Ambient.Set(FbxDouble3(ambient.R, ambient.G, ambient.B));
								pMat->AmbientFactor.Set(FbxDouble(ambient.A));
								pMat->Emissive.Set(FbxDouble3(emissive.R, emissive.G, emissive.B));
								pMat->EmissiveFactor.Set(FbxDouble(emissive.A));
								pMat->Specular.Set(FbxDouble3(specular.R, specular.G, specular.B));
								pMat->SpecularFactor.Set(FbxDouble(specular.A));
								pMat->Reflection.Set(FbxDouble3(reflection.R, reflection.G, reflection.B));
								pMat->ReflectionFactor.Set(FbxDouble(reflection.A));
								pMat->Shininess.Set(FbxDouble(mat->Shininess));
								pMat->TransparencyFactor.Set(FbxDouble(mat->Transparency));
								pMat->ShadingModel.Set(lShadingName);
								foundMat = pMaterials->GetCount();
								pMaterials->Add(pMat);
							}
							pMeshNode->AddMaterial(pMat);

							bool hasTexture = false;

							for each (ImportedMaterialTexture^ texture in mat->Textures)
							{
								auto pTexture = ExportTexture(ImportedHelpers::FindTexture(texture->Name, imported->TextureList));
								if (pTexture != NULL)
								{
									if (texture->Dest == 0)
									{
										LinkTexture(texture, pTexture, pMat->Diffuse);
										hasTexture = true;
									}
									else if (texture->Dest == 1)
									{
										LinkTexture(texture, pTexture, pMat->NormalMap);
										hasTexture = true;
									}
									else if (texture->Dest == 2)
									{
										LinkTexture(texture, pTexture, pMat->Specular);
										hasTexture = true;
									}
									else if (texture->Dest == 3)
									{
										LinkTexture(texture, pTexture, pMat->Bump);
										hasTexture = true;
									}
								}
							}

							if (hasTexture)
							{
								pMeshNode->SetShadingMode(FbxNode::eTextureShading);
							}
						}
						finally
						{
							Marshal::FreeHGlobal((IntPtr)pMatName);
						}
					}

					for (int j = 0; j < vertexList->Count; j++)
					{
						ImportedVertex^ vertex = vertexList[j];
						Vector3 coords = vertex->Position;
						pControlPoints[j] = FbxVector4(coords.X, coords.Y, coords.Z, 0);
						//if (normals)
						{
							Vector3 normal = vertex->Normal;
							lGeometryElementNormal->GetDirectArray().Add(FbxVector4(normal.X, normal.Y, normal.Z, 0));
						}
						array<float>^ uv = vertex->UV;
						if (uv != nullptr)
							lGeometryElementUV->GetDirectArray().Add(FbxVector2(uv[0], uv[1]));
						if (normals)
						{
							Vector4 tangent = vertex->Tangent;
							lGeometryElementTangent->GetDirectArray().Add(FbxVector4(tangent.X, tangent.Y, tangent.Z, tangent.W));
						}

						if (hasBones && vertex->BoneIndices != nullptr)
						{
							auto boneIndices = vertex->BoneIndices;
							auto weights4 = vertex->Weights;
							for (int k = 0; k < weights4->Length; k++)
							{
								if (boneIndices[k] < boneList->Count && weights4[k] > 0)
								{
									FbxCluster* pCluster = pClusterArray->GetAt(boneIndices[k]);
									pCluster->AddControlPointIndex(j, weights4[k]);
								}
							}
						}
					}

					for (int j = 0; j < faceList->Count; j++)
					{
						ImportedFace^ face = faceList[j];
						pMesh->BeginPolygon(0);
						pMesh->AddPolygon(face->VertexIndices[0]);
						pMesh->AddPolygon(face->VertexIndices[1]);
						pMesh->AddPolygon(face->VertexIndices[2]);
						pMesh->EndPolygon();
					}

					if (hasBones)
					{
						FbxSkin* pSkin = FbxSkin::Create(pScene, "");
						FbxAMatrix lMeshMatrix = pMeshNode->EvaluateGlobalTransform();
						for (int j = 0; j < boneList->Count; j++)
						{
							FbxCluster* pCluster = pClusterArray->GetAt(j);
							if (pCluster->GetControlPointIndicesCount() > 0)
							{
								auto boneMatrix = boneList[j]->Matrix;
								FbxAMatrix lBoneMatrix;
								for (int m = 0; m < 4; m++)
								{
									for (int n = 0; n < 4; n++)
									{
										lBoneMatrix.mData[m][n] = boneMatrix[m, n];
									}
								}

								pCluster->SetTransformMatrix(lMeshMatrix);
								pCluster->SetTransformLinkMatrix(lMeshMatrix * lBoneMatrix.Inverse());

								pSkin->AddCluster(pCluster);
							}
						}

						if (pSkin->GetClusterCount() > 0)
						{
							pMesh->AddDeformer(pSkin);
						}
					}
				}
				finally
				{
					if (pClusterArray != NULL)
					{
						delete pClusterArray;
					}
					Marshal::FreeHGlobal((IntPtr)pName);
				}
			}
		}
		finally
		{
			if (pBoneNodeList != NULL)
			{
				delete pBoneNodeList;
			}
		}
	}

	FbxNode* Fbx::Exporter::FindNodeByPath(String ^ path, bool recursive)
	{
		array<String^>^ splitPath = path->Split('/');
		FbxNode* lNode = pScene->GetRootNode();
		for (int i = 0; i < splitPath->Length; i++)
		{
			String^ frameName = splitPath[i];
			char* pNodeName = NULL;
			try
			{
				pNodeName = StringToCharArray(frameName);
				FbxNode* foundNode;
				if (recursive && i == 0)
				{
					foundNode = lNode->FindChild(pNodeName);
				}
				else
				{
					foundNode = lNode->FindChild(pNodeName, false);
				}
				if (foundNode == NULL)
				{
					//throw gcnew Exception(gcnew String("Couldn't find path ") + path);
					return NULL;
				}
				lNode = foundNode;
			}
			finally
			{
				Marshal::FreeHGlobal((IntPtr)pNodeName);
			}
		}
		return lNode;
	}

	FbxFileTexture* Fbx::Exporter::ExportTexture(ImportedTexture^ matTex)
	{
		FbxFileTexture* pTex = NULL;

		if (matTex != nullptr)
		{
			String^ matTexName = matTex->Name;
			char* pTexName = NULL;
			try
			{
				pTexName = StringToCharArray(matTexName);
				int foundTex = -1;
				for (int i = 0; i < pTextures->GetCount(); i++)
				{
					FbxFileTexture* pTexTemp = pTextures->GetAt(i);
					if (strcmp(pTexTemp->GetName(), pTexName) == 0)
					{
						foundTex = i;
						break;
					}
				}

				if (foundTex >= 0)
				{
					pTex = pTextures->GetAt(foundTex);
				}
				else
				{
					pTex = FbxFileTexture::Create(pScene, pTexName);
					pTex->SetFileName(pTexName);
					pTex->SetTextureUse(FbxTexture::eStandard);
					pTex->SetMappingType(FbxTexture::eUV);
					pTex->SetMaterialUse(FbxFileTexture::eModelMaterial);
					pTex->SetSwapUV(false);
					pTex->SetTranslation(0.0, 0.0);
					pTex->SetScale(1.0, 1.0);
					pTex->SetRotation(0.0, 0.0);
					pTextures->Add(pTex);

					String^ path = Path::GetDirectoryName(gcnew String(pExporter->GetFileName().Buffer()));
					if (path == String::Empty)
					{
						path = ".";
					}
					FileInfo^ file = gcnew FileInfo(path + Path::DirectorySeparatorChar + Path::GetFileName(matTex->Name));
					DirectoryInfo^ dir = file->Directory;
					if (!dir->Exists)
					{
						dir->Create();
					}
					BinaryWriter^ writer = gcnew BinaryWriter(file->Create());
					writer->Write(matTex->Data);
					writer->Close();
				}
			}
			finally
			{
				Marshal::FreeHGlobal((IntPtr)pTexName);
			}
		}

		return pTex;
	}

	void Fbx::Exporter::LinkTexture(ImportedMaterialTexture^ texture, FbxFileTexture* pTexture, FbxProperty& prop)
	{
		pTexture->SetTranslation(texture->Offset.X, texture->Offset.Y);
		pTexture->SetScale(texture->Scale.X, texture->Scale.Y);
		prop.ConnectSrcObject(pTexture);
	}

	void Fbx::Exporter::ExportAnimations(bool eulerFilter, float filterPrecision, bool flatInbetween)
	{
		auto importedAnimationList = imported->AnimationList;
		if (importedAnimationList == nullptr)
		{
			return;
		}

		FbxAnimCurveFilterUnroll* lFilter = eulerFilter ? new FbxAnimCurveFilterUnroll() : NULL;

		for (int i = 0; i < importedAnimationList->Count; i++)
		{
			auto importedAnimation = importedAnimationList[i];
			FbxString kTakeName;
			if (importedAnimation->Name)
			{
				WITH_MARSHALLED_STRING
				(
					pClipName,
					importedAnimation->Name,
					kTakeName = FbxString(pClipName);
				);
			}
			else
			{
				kTakeName = FbxString("Take") + FbxString(i);
			}
			ExportKeyframedAnimation(importedAnimation, kTakeName, lFilter, filterPrecision, flatInbetween);
		}
	}

	void Fbx::Exporter::ExportKeyframedAnimation(ImportedKeyframedAnimation^ parser, FbxString& kTakeName, FbxAnimCurveFilterUnroll* eulerFilter, float filterPrecision, bool flatInbetween)
	{
		List<ImportedAnimationKeyframedTrack^>^ pAnimationList = parser->TrackList;

		char* lTakeName = kTakeName.Buffer();

		FbxAnimStack* lAnimStack = FbxAnimStack::Create(pScene, lTakeName);
		FbxAnimLayer* lAnimLayer = FbxAnimLayer::Create(pScene, "Base Layer");
		lAnimStack->AddMember(lAnimLayer);

		for (int j = 0; j < pAnimationList->Count; j++)
		{
			ImportedAnimationKeyframedTrack^ keyframeList = pAnimationList[j];
			FbxNode* pNode = FindNodeByPath(keyframeList->Path, true);
			if (pNode != nullptr)
			{
				FbxAnimCurve* lCurveSX = pNode->LclScaling.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_X, true);
				FbxAnimCurve* lCurveSY = pNode->LclScaling.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_Y, true);
				FbxAnimCurve* lCurveSZ = pNode->LclScaling.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_Z, true);
				FbxAnimCurve* lCurveRX = pNode->LclRotation.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_X, true);
				FbxAnimCurve* lCurveRY = pNode->LclRotation.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_Y, true);
				FbxAnimCurve* lCurveRZ = pNode->LclRotation.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_Z, true);
				FbxAnimCurve* lCurveTX = pNode->LclTranslation.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_X, true);
				FbxAnimCurve* lCurveTY = pNode->LclTranslation.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_Y, true);
				FbxAnimCurve* lCurveTZ = pNode->LclTranslation.GetCurve(lAnimLayer, FBXSDK_CURVENODE_COMPONENT_Z, true);

				lCurveSX->KeyModifyBegin();
				lCurveSY->KeyModifyBegin();
				lCurveSZ->KeyModifyBegin();
				lCurveRX->KeyModifyBegin();
				lCurveRY->KeyModifyBegin();
				lCurveRZ->KeyModifyBegin();
				lCurveTX->KeyModifyBegin();
				lCurveTY->KeyModifyBegin();
				lCurveTZ->KeyModifyBegin();

				FbxTime lTime;

				for each (auto Scaling in keyframeList->Scalings)
				{
					lTime.SetSecondDouble(Scaling->time);

					lCurveSX->KeySet(lCurveSX->KeyAdd(lTime), lTime, Scaling->value.X);
					lCurveSY->KeySet(lCurveSY->KeyAdd(lTime), lTime, Scaling->value.Y);
					lCurveSZ->KeySet(lCurveSZ->KeyAdd(lTime), lTime, Scaling->value.Z);
				}
				for each (auto Rotation in keyframeList->Rotations)
				{
					lTime.SetSecondDouble(Rotation->time);

					lCurveRX->KeySet(lCurveRX->KeyAdd(lTime), lTime, Rotation->value.X);
					lCurveRY->KeySet(lCurveRY->KeyAdd(lTime), lTime, Rotation->value.Y);
					lCurveRZ->KeySet(lCurveRZ->KeyAdd(lTime), lTime, Rotation->value.Z);
				}
				for each (auto Translation in keyframeList->Translations)
				{
					lTime.SetSecondDouble(Translation->time);

					lCurveTX->KeySet(lCurveTX->KeyAdd(lTime), lTime, Translation->value.X);
					lCurveTY->KeySet(lCurveTY->KeyAdd(lTime), lTime, Translation->value.Y);
					lCurveTZ->KeySet(lCurveTZ->KeyAdd(lTime), lTime, Translation->value.Z);
				}

				lCurveSX->KeyModifyEnd();
				lCurveSY->KeyModifyEnd();
				lCurveSZ->KeyModifyEnd();
				lCurveRX->KeyModifyEnd();
				lCurveRY->KeyModifyEnd();
				lCurveRZ->KeyModifyEnd();
				lCurveTX->KeyModifyEnd();
				lCurveTY->KeyModifyEnd();
				lCurveTZ->KeyModifyEnd();

				if (eulerFilter)
				{
					FbxAnimCurve* lCurve[3];
					lCurve[0] = lCurveRX;
					lCurve[1] = lCurveRY;
					lCurve[2] = lCurveRZ;
					eulerFilter->Reset();
					eulerFilter->SetQualityTolerance(filterPrecision);
					eulerFilter->Apply(lCurve, 3);
				}
			}
		}
	}

	void Fbx::Exporter::ExportMorphs(IImported^ imported, bool morphMask, bool flatInbetween)
	{
		if (imported->MeshList == nullptr)
		{
			return;
		}

		for (int meshIdx = 0; meshIdx < imported->MeshList->Count; meshIdx++)
		{
			ImportedMesh^ meshList = imported->MeshList[meshIdx];
			FbxNode* pBaseNode = NULL;
			for (int nodeIdx = 0; nodeIdx < pMeshNodes->GetCount(); nodeIdx++)
			{
				FbxNode* pMeshNode = pMeshNodes->GetAt(nodeIdx);
				String^ framePath = gcnew String(pMeshNode->GetName());
				FbxNode* rootNode = pMeshNode;
				while ((rootNode = rootNode->GetParent()) != pScene->GetRootNode())
				{
					framePath = gcnew String(rootNode->GetName()) + "/" + framePath;
				}
				if (framePath == meshList->Path)
				{
					pBaseNode = pMeshNode;
					break;
				}
			}
			if (pBaseNode == NULL)
			{
				continue;
			}

			for each (ImportedMorph^ morph in imported->MorphList)
			{
				if (morph->Path != meshList->Path)
				{
					continue;
				}

				int meshVertexIndex = 0;
				for (int meshObjIdx = pBaseNode->GetChildCount() - meshList->SubmeshList->Count; meshObjIdx < meshList->SubmeshList->Count; meshObjIdx++)
				{
					List<ImportedVertex^>^ vertList = meshList->SubmeshList[meshObjIdx]->VertexList;
					FbxNode* pBaseMeshNode = pBaseNode->GetChild(meshObjIdx);
					FbxMesh* pBaseMesh = pBaseMeshNode->GetMesh();

					FbxBlendShape* lBlendShape;
					WITH_MARSHALLED_STRING
					(
						pShapeName,
						morph->ClipName + (meshList->SubmeshList->Count > 1 ? "_" + meshObjIdx : String::Empty) /*+ "_BlendShape"*/,
						lBlendShape = FbxBlendShape::Create(pScene, pShapeName);
					);
					pBaseMesh->AddDeformer(lBlendShape);
					List<ImportedMorphKeyframe^>^ keyframes = morph->KeyframeList;
					for (int i = 0; i < morph->Channels->Count; i++)
					{
						FbxBlendShapeChannel* lBlendShapeChannel;
						if (!flatInbetween)
						{
							WITH_MARSHALLED_STRING
							(
								pChannelName,
								gcnew String(lBlendShape->GetName()) + "." + keyframes[morph->Channels[i]->Item2]->Name->Substring(0, keyframes[morph->Channels[i]->Item2]->Name->LastIndexOf("_")),
								lBlendShapeChannel = FbxBlendShapeChannel::Create(pScene, pChannelName);
							);
							lBlendShapeChannel->DeformPercent = morph->Channels[i]->Item1;
							lBlendShape->AddBlendShapeChannel(lBlendShapeChannel);
						}

						for (int frameIdx = 0; frameIdx < morph->Channels[i]->Item3; frameIdx++)
						{
							int shapeIdx = morph->Channels[i]->Item2 + frameIdx;
							ImportedMorphKeyframe^ keyframe = keyframes[shapeIdx];

							FbxShape* pShape;
							if (!flatInbetween)
							{
								WITH_MARSHALLED_STRING
								(
									pMorphShapeName,
									keyframe->Name,
									pShape = FbxShape::Create(pScene, pMorphShapeName);
								);
								lBlendShapeChannel->AddTargetShape(pShape, keyframe->Weight);
							}
							else
							{
								lBlendShapeChannel = FbxBlendShapeChannel::Create(pScene, "");
								lBlendShapeChannel->DeformPercent = morph->Channels[i]->Item1;
								lBlendShape->AddBlendShapeChannel(lBlendShapeChannel);

								WITH_MARSHALLED_STRING
								(
									pMorphShapeName,
									morph->ClipName + (meshList->SubmeshList->Count > 1 ? "_" + meshObjIdx : String::Empty) + "." + keyframe->Name,
									pShape = FbxShape::Create(pScene, pMorphShapeName);
								);
								lBlendShapeChannel->AddTargetShape(pShape, 100);

								FbxProperty weightProp;
								WITH_MARSHALLED_STRING
								(
									pWeightName,
									gcnew String(pShape->GetName()) + ".Weight",
									weightProp = FbxProperty::Create(pBaseMesh, FbxDoubleDT, pWeightName);
								);
								weightProp.ModifyFlag(FbxPropertyFlags::eUserDefined, true);
								weightProp.Set<double>(keyframe->Weight);
							}

							pShape->InitControlPoints(vertList->Count);
							FbxVector4* pControlPoints = pShape->GetControlPoints();

							for (int j = 0; j < vertList->Count; j++)
							{
								ImportedVertex^ vertex = vertList[j];
								Vector3 coords = vertex->Position;
								pControlPoints[j] = FbxVector4(coords.X, coords.Y, coords.Z, 0);
							}
							List<unsigned short>^ meshIndices = keyframe->MorphedVertexIndices;
							for (int j = 0; j < meshIndices->Count; j++)
							{
								int controlPointIndex = meshIndices[j] - meshVertexIndex;
								if (controlPointIndex >= 0 && controlPointIndex < vertList->Count)
								{
									Vector3 coords = keyframe->VertexList[j]->Position;
									pControlPoints[controlPointIndex] = FbxVector4(coords.X, coords.Y, coords.Z, 0);
								}
							}

							if (flatInbetween && frameIdx > 0)
							{
								int shapeIdx = morph->Channels[i]->Item2 + frameIdx - 1;
								ImportedMorphKeyframe^ keyframe = keyframes[shapeIdx];

								List<unsigned short>^ meshIndices = keyframe->MorphedVertexIndices;
								for (int j = 0; j < meshIndices->Count; j++)
								{
									int controlPointIndex = meshIndices[j] - meshVertexIndex;
									if (controlPointIndex >= 0 && controlPointIndex < vertList->Count)
									{
										Vector3 coords = keyframe->VertexList[j]->Position - vertList[controlPointIndex]->Position;
										pControlPoints[controlPointIndex] -= FbxVector4(coords.X, coords.Y, coords.Z, 0);
									}
								}
							}

							if (morphMask)
							{
								FbxGeometryElementVertexColor* lGeometryElementVertexColor = pBaseMesh->CreateElementVertexColor();
								lGeometryElementVertexColor->SetMappingMode(FbxGeometryElement::eByControlPoint);
								lGeometryElementVertexColor->SetReferenceMode(FbxGeometryElement::eDirect);
								WITH_MARSHALLED_STRING
								(
									pColourLayerName, morph->KeyframeList[shapeIdx]->Name,
									lGeometryElementVertexColor->SetName(pColourLayerName);
								);
								for (int j = 0; j < vertList->Count; j++)
								{
									lGeometryElementVertexColor->GetDirectArray().Add(FbxColor(1, 1, 1));
								}
								for (int j = 0; j < meshIndices->Count; j++)
								{
									int controlPointIndex = meshIndices[j] - meshVertexIndex;
									if (controlPointIndex >= 0 && controlPointIndex < vertList->Count)
									{
										lGeometryElementVertexColor->GetDirectArray().SetAt(controlPointIndex, FbxColor(0, 0, 1));
									}
								}
							}
						}
					}
					meshVertexIndex += meshList->SubmeshList[meshObjIdx]->VertexList->Count;
				}
			}
		}
	}
}