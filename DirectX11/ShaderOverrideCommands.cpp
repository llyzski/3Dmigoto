#include "ShaderOverrideCommands.h"

void RunShaderOverrideCommandList(HackerDevice *mHackerDevice,
		HackerContext *mHackerContext,
		ShaderOverrideCommandList *command_list)
{
	ShaderOverrideCommandList::iterator i;
	ShaderOverrideState state;
	ID3D11Device *mOrigDevice = mHackerDevice->GetOrigDevice();
	ID3D11DeviceContext *mOrigContext = mHackerContext->GetOrigContext();
	D3D11_MAPPED_SUBRESOURCE mappedResource;

	for (i = command_list->begin(); i < command_list->end(); i++) {
		(*i)->run(mHackerContext, mOrigDevice, mOrigContext, &state);
	}

	if (state.update_params) {
		mOrigContext->Map(mHackerDevice->mIniTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
		memcpy(mappedResource.pData, &G->iniParams, sizeof(G->iniParams));
		mOrigContext->Unmap(mHackerDevice->mIniTexture, 0);
	}
}


static void ProcessParamRTSize(ID3D11DeviceContext *mOrigContext, ShaderOverrideState *state)
{
	D3D11_RENDER_TARGET_VIEW_DESC view_desc;
	D3D11_TEXTURE2D_DESC res_desc;
	ID3D11RenderTargetView *view = NULL;
	ID3D11Resource *res = NULL;
	ID3D11Texture2D *tex = NULL;

	if (state->rt_width != -1)
		return;

	mOrigContext->OMGetRenderTargets(1, &view, NULL);
	if (!view)
		return;

	view->GetDesc(&view_desc);

	if (view_desc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D &&
	    view_desc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2DMS)
		goto out_release_view;

	view->GetResource(&res);
	if (!res)
		goto out_release_view;

	tex = (ID3D11Texture2D *)res;
	tex->GetDesc(&res_desc);

	state->rt_width = (float)res_desc.Width;
	state->rt_height = (float)res_desc.Height;

	tex->Release();
out_release_view:
	view->Release();
}

static float ProcessParamTextureFilter(HackerContext *mHackerContext,
		ID3D11DeviceContext *mOrigContext, ParamOverride *override)
{
	D3D11_SHADER_RESOURCE_VIEW_DESC desc;
	ID3D11ShaderResourceView *view;
	ID3D11Resource *resource = NULL;
	TextureOverrideMap::iterator i;
	uint32_t hash = 0;
	float filter_index = 0;

	switch (override->shader_type) {
		case L'v':
			mOrigContext->VSGetShaderResources(override->texture_slot, 1, &view);
			break;
		case L'h':
			mOrigContext->HSGetShaderResources(override->texture_slot, 1, &view);
			break;
		case L'd':
			mOrigContext->DSGetShaderResources(override->texture_slot, 1, &view);
			break;
		case L'g':
			mOrigContext->GSGetShaderResources(override->texture_slot, 1, &view);
			break;
		case L'p':
			mOrigContext->PSGetShaderResources(override->texture_slot, 1, &view);
			break;
		default:
			// Should not happen
			return filter_index;
	}
	if (!view)
		return filter_index;


	view->GetResource(&resource);
	if (!resource)
		goto out_release_view;

	view->GetDesc(&desc);

	switch (desc.ViewDimension) {
		case D3D11_SRV_DIMENSION_TEXTURE2D:
		case D3D11_SRV_DIMENSION_TEXTURE2DMS:
		case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
			hash = mHackerContext->GetTexture2DHash((ID3D11Texture2D *)resource, false, NULL);
			break;
		case D3D11_SRV_DIMENSION_TEXTURE3D:
			hash = mHackerContext->GetTexture3DHash((ID3D11Texture3D *)resource, false, NULL);
			break;
	}
	if (!hash)
		goto out_release_resource;

	i = G->mTextureOverrideMap.find(hash);
	if (i == G->mTextureOverrideMap.end())
		goto out_release_resource;

	filter_index = i->second.filter_index;

out_release_resource:
	resource->Release();
out_release_view:
	view->Release();
	return filter_index;
}

void ParamOverride::run(HackerContext *mHackerContext, ID3D11Device *mOrigDevice,
		ID3D11DeviceContext *mOrigContext, ShaderOverrideState *state)
{
	float *dest = &(G->iniParams[param_idx].*param_component);

	float orig = *dest;

	switch (type) {
		case ParamOverrideType::VALUE:
			*dest = val;
			break;
		case ParamOverrideType::RT_WIDTH:
			ProcessParamRTSize(mOrigContext, state);
			*dest = state->rt_width;
			break;
		case ParamOverrideType::RT_HEIGHT:
			ProcessParamRTSize(mOrigContext, state);
			*dest = state->rt_height;
			break;
		case ParamOverrideType::RES_WIDTH:
			*dest = (float)G->mResolutionInfo.width;
			break;
		case ParamOverrideType::RES_HEIGHT:
			*dest = (float)G->mResolutionInfo.height;
			break;
		case ParamOverrideType::TEXTURE:
			*dest = ProcessParamTextureFilter(mHackerContext,
					mOrigContext, this);
			break;
		default:
			return;
	}
	state->update_params |= (*dest != orig);
}

// Parse IniParams overrides, in forms such as
// x = 0.3 (set parameter to specific value, e.g. for shader partner filtering)
// y2 = ps-t0 (use parameter for texture filtering based on texture slot of shader type)
// z3 = rt_width / rt_height (set parameter to render target width/height)
// w4 = res_width / res_height (set parameter to resolution width/height)
bool ParseShaderOverrideIniParamOverride(wstring *key, wstring *val,
		ShaderOverrideCommandList *command_list)
{
	int ret, len1, len2;
	wchar_t component;
	ParamOverride *param = new ParamOverride();

	// Parse key
	ret = swscanf_s(key->data(), L"%lc%n%u%n", &component, 1, &len1, &param->param_idx, &len2);

	// May or may not have matched index. Make sure entire string was
	// matched either way and check index is valid if it was matched:
	if (ret == 1 && len1 == key->length()) {
		param->param_idx = 0;
	} else if (ret == 2 && len2 == key->length()) {
		if (param->param_idx >= INI_PARAMS_SIZE)
			goto bail;
	} else {
		goto bail;
	}

	switch (component) {
		case L'x':
			param->param_component = &DirectX::XMFLOAT4::x;
			break;
		case L'y':
			param->param_component = &DirectX::XMFLOAT4::y;
			break;
		case L'z':
			param->param_component = &DirectX::XMFLOAT4::z;
			break;
		case L'w':
			param->param_component = &DirectX::XMFLOAT4::w;
			break;
		default:
			goto bail;
	}

	// Try parsing value as a float
	ret = swscanf_s(val->data(), L"%f%n", &param->val, &len1);
	if (ret != 0 && ret != EOF && len1 == val->length()) {
		param->type = ParamOverrideType::VALUE;
		goto success;
	}

	// Try parsing value as "<shader type>s-t<testure slot>" for texture filtering
	ret = swscanf_s(val->data(), L"%lcs-t%u%n", &param->shader_type, 1, &param->texture_slot, &len1);
	if (ret == 2 && len1 == val->length() &&
			param->texture_slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
		switch(param->shader_type) {
			case L'v': case L'h': case L'd': case L'g': case L'p':
				param->type = ParamOverrideType::TEXTURE;
				goto success;
			default:
				goto bail;
		}
	}

	// Check special keywords
	param->type = lookup_enum_val<const wchar_t *, ParamOverrideType>
		(ParamOverrideTypeNames, val->data(), ParamOverrideType::INVALID);
	if (param->type == ParamOverrideType::INVALID)
		goto bail;

success:
	LogInfoW(L"  %ls=%s\n", key->data(), val->data());
	command_list->push_back(std::unique_ptr<ShaderOverrideCommand>(param));
	return true;
bail:
	delete param;
	return false;
}


bool ResourceCopyTarget::ParseTarget(const wchar_t *target, bool allow_null)
{
	int ret, len;
	size_t length = wcslen(target);

	ret = swscanf_s(target, L"%lcs-cb%u%n", &shader_type, 1, &slot, &len);
	if (ret == 2 && len == length && slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) {
		type = ResourceCopyTargetType::CONSTANT_BUFFER;
		goto check_shader_type;
	}

	ret = swscanf_s(target, L"%lcs-t%u%n", &shader_type, 1, &slot, &len);
	if (ret == 2 && len == length && slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
		type = ResourceCopyTargetType::SHADER_RESOURCE;
	       goto check_shader_type;
	}

	// TODO: ret = swscanf_s(target, L"%lcs-s%u%n", &shader_type, 1, &slot, &len);
	// TODO: if (ret == 2 && len == length && slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
	// TODO: 	type = ResourceCopyTargetType::SAMPLER;
	// TODO:	goto check_shader_type;
	// TODO: }

	ret = swscanf_s(target, L"o%u%n", &slot, &len);
	if (ret == 1 && len == length && slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT) {
		type = ResourceCopyTargetType::RENDER_TARGET;
		return true;
	}

	if (!wcscmp(target, L"oD")) {
		type = ResourceCopyTargetType::DEPTH_STENCIL_TARGET;
		return true;
	}

	// TODO: ret = swscanf_s(target, L"u%u%n", &slot, &len);
	// TODO: // XXX: On Win8 D3D11_1_UAV_SLOT_COUNT (64) is the limit instead. Use
	// TODO: // the lower amount for now to enforce compatibility.
	// TODO: if (ret == 1 && len == length && slot < D3D11_PS_CS_UAV_REGISTER_COUNT) {
	// TODO: 	type = ResourceCopyTargetType::UNORDERED_ACCESS_VIEW;
	// TODO: 	return true;
	// TODO: }

	// TODO: ret = swscanf_s(target, L"vb%u%n", &slot, &len);
	// TODO: if (ret == 1 && len == length && slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) {
	// TODO: 	type = ResourceCopyTargetType::VERTEX_BUFFER;
	// TODO: 	return true;
	// TODO: }

	// TODO: if (!wcscmp(target, L"ib")) {
	// TODO: 	type = ResourceCopyTargetType::INDEX_BUFFER;
	// TODO: 	return true;
	// TODO: }

	// TODO: ret = swscanf_s(target, L"so%u%n", &slot, &len);
	// TODO: if (ret == 1 && len == length && slot < D3D11_SO_STREAM_COUNT) {
	// TODO: 	type = ResourceCopyTargetType::STREAM_OUTPUT;
	// TODO: 	return true;
	// TODO: }

	if (allow_null && !wcscmp(target, L"null")) {
		type = ResourceCopyTargetType::EMPTY;
		return true;
	}

	return false;

check_shader_type:
	switch(shader_type) {
		case L'v': case L'h': case L'd': case L'g': case L'p': case L'c':
			return true;
	}
	return false;
}


bool ParseShaderOverrideResourceCopyDirective(wstring *key, wstring *val,
		ShaderOverrideCommandList *command_list)
{
	ResourceCopyOperation *operation = new ResourceCopyOperation();
	const wchar_t *src_ptr = val->data();

	if (!operation->dst.ParseTarget(key->data(), false))
		goto bail;

	if (!val->compare(0, 5, L"copy ")) {
		operation->copy_type = ResourceCopyOperationType::COPY;
		src_ptr += 5;
	} else if (!val->compare(0, 10, L"reference ")) {
		operation->copy_type = ResourceCopyOperationType::REFERENCE;
		src_ptr += 10;
	} else if (!val->compare(0, 4, L"ref ")) {
		operation->copy_type = ResourceCopyOperationType::REFERENCE;
		src_ptr += 4;
	}

	if (!operation->src.ParseTarget(src_ptr, true))
		goto bail;

	if (operation->copy_type == ResourceCopyOperationType::AUTO) {
		// If the copy method was not speficied make a guess.
		// References aren't always safe (e.g. a resource can't be both
		// an input and an output), and a resource may not have been
		// created with the right usage flags, so we'll err on the side
		// of doing a full copy if we aren't fairly sure.
		//
		// If we're merely copying a resource from one shader to
		// another without changnig the usage (e.g. giving the vertex
		// shader access to a constant buffer or texture from the pixel
		// shader) a reference is probably safe (unless the game
		// reassigns it to a different usage later and doesn't know
		// that our reference is still bound somewhere), but it would
		// not be safe to give a vertex shader access to the depth
		// buffer of the output merger stage, for example.
		//
		// TODO: If we are copying a resource into a custom resource
		// (e.g. for use from another draw call), do a full copy by
		// default in case the game alters the original.
		// if (dst.type == ResourceCopyTargetType::CUSTOM_RESOURCE)
		// 	operation->copy_type = ResourceCopyOperationType::COPY;
		// else if (operation->src.type == ResourceCopyTargetType::CUSTOM_RESOURCE)
		// 	operation->copy_type = ResourceCopyOperationType::REFERENCE;
		// else
		if (operation->src.type == operation->dst.type)
			operation->copy_type = ResourceCopyOperationType::REFERENCE;
		else
			operation->copy_type = ResourceCopyOperationType::COPY;
	}

	LogInfoW(L"  %ls=%s\n", key->data(), val->data());
	command_list->push_back(std::unique_ptr<ShaderOverrideCommand>(operation));
	return true;
bail:
	delete operation;
	return false;
}

ID3D11Resource *ResourceCopyTarget::GetResource(ID3D11DeviceContext *mOrigContext, ID3D11View **view)
{
	ID3D11Resource *res = NULL;
	ID3D11Buffer *buf = NULL;
	ID3D11ShaderResourceView *resource_view = NULL;
	ID3D11RenderTargetView *render_view[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
	ID3D11DepthStencilView *depth_view = NULL;
	unsigned i;

	*view = NULL;

	switch(type) {
	case ResourceCopyTargetType::CONSTANT_BUFFER:
		// FIXME: On win8 (or with evil update?), we should use
		// Get/SetConstantBuffers1 and copy the offset into the buffer as well
		switch(shader_type) {
		case L'v':
			mOrigContext->VSGetConstantBuffers(slot, 1, &buf);
			return buf;
		case L'h':
			mOrigContext->HSGetConstantBuffers(slot, 1, &buf);
			return buf;
		case L'd':
			mOrigContext->DSGetConstantBuffers(slot, 1, &buf);
			return buf;
		case L'g':
			mOrigContext->GSGetConstantBuffers(slot, 1, &buf);
			return buf;
		case L'p':
			mOrigContext->PSGetConstantBuffers(slot, 1, &buf);
			return buf;
		case L'c':
			mOrigContext->CSGetConstantBuffers(slot, 1, &buf);
			return buf;
		default:
			// Should not happen
			return NULL;
		}
		break;

	case ResourceCopyTargetType::SHADER_RESOURCE:
		switch(shader_type) {
		case L'v':
			mOrigContext->VSGetShaderResources(slot, 1, &resource_view);
			break;
		case L'h':
			mOrigContext->HSGetShaderResources(slot, 1, &resource_view);
			break;
		case L'd':
			mOrigContext->DSGetShaderResources(slot, 1, &resource_view);
			break;
		case L'g':
			mOrigContext->GSGetShaderResources(slot, 1, &resource_view);
			break;
		case L'p':
			mOrigContext->PSGetShaderResources(slot, 1, &resource_view);
			break;
		case L'c':
			mOrigContext->CSGetShaderResources(slot, 1, &resource_view);
			break;
		default:
			// Should not happen
			return NULL;
		}

		if (!resource_view)
			return NULL;

		resource_view->GetResource(&res);
		if (!res) {
			resource_view->Release();
			return NULL;
		}

		*view = resource_view;
		return res;

	// TODO: case ResourceCopyTargetType::SAMPLER: // Not an ID3D11Resource, need to think about this one
	// TODO: 	break;
	// TODO: case ResourceCopyTargetType::VERTEX_BUFFER:
	// TODO: 	break;
	// TODO: case ResourceCopyTargetType::INDEX_BUFFER:
	// TODO: 	break;
	// TODO: case ResourceCopyTargetType::STREAM_OUTPUT:
	// TODO: 	break;

	case ResourceCopyTargetType::RENDER_TARGET:
		mOrigContext->OMGetRenderTargets(slot + 1, render_view, NULL);

		// Release any views we aren't after:
		for (i = 0; i < slot; i++) {
			if (render_view[i]) {
				render_view[i]->Release();
				render_view[i] = NULL;
			}
		}

		if (!render_view[slot])
			return NULL;

		render_view[slot]->GetResource(&res);
		if (!res) {
			render_view[slot]->Release();
			return NULL;
		}

		*view = render_view[slot];
		return res;

	case ResourceCopyTargetType::DEPTH_STENCIL_TARGET:
		mOrigContext->OMGetRenderTargets(0, NULL, &depth_view);
		if (!depth_view)
			return NULL;

		depth_view->GetResource(&res);
		if (!res) {
			depth_view->Release();
			return NULL;
		}

		*view = depth_view;
		return res;

	// TODO: case ResourceCopyTargetType::UNORDERED_ACCESS_VIEW:
	// TODO: 	break;
	// TODO: case ResourceCopyTargetType::CUSTOM_RESOURCE:
	// TODO: 	break;
	}

	return NULL;
}

void ResourceCopyTarget::SetResource(ID3D11DeviceContext *mOrigContext, ID3D11Resource *res, ID3D11View *view)
{
	ID3D11Buffer *buf = NULL;
	ID3D11ShaderResourceView *resource_view = NULL;
	ID3D11RenderTargetView *render_view[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
	ID3D11DepthStencilView *depth_view = NULL;
	int i;

	switch(type) {
	case ResourceCopyTargetType::CONSTANT_BUFFER:
		// FIXME: On win8 (or with evil update?), we should use
		// Get/SetConstantBuffers1 and copy the offset into the buffer as well
		buf = (ID3D11Buffer*)res;
		switch(shader_type) {
		case L'v':
			mOrigContext->VSSetConstantBuffers(slot, 1, &buf);
			return;
		case L'h':
			mOrigContext->HSSetConstantBuffers(slot, 1, &buf);
			return;
		case L'd':
			mOrigContext->DSSetConstantBuffers(slot, 1, &buf);
			return;
		case L'g':
			mOrigContext->GSSetConstantBuffers(slot, 1, &buf);
			return;
		case L'p':
			mOrigContext->PSSetConstantBuffers(slot, 1, &buf);
			return;
		case L'c':
			mOrigContext->CSSetConstantBuffers(slot, 1, &buf);
			return;
		default:
			// Should not happen
			return;
		}
		break;

	case ResourceCopyTargetType::SHADER_RESOURCE:
		resource_view = (ID3D11ShaderResourceView*)view;
		switch(shader_type) {
		case L'v':
			mOrigContext->VSSetShaderResources(slot, 1, &resource_view);
			break;
		case L'h':
			mOrigContext->HSSetShaderResources(slot, 1, &resource_view);
			break;
		case L'd':
			mOrigContext->DSSetShaderResources(slot, 1, &resource_view);
			break;
		case L'g':
			mOrigContext->GSSetShaderResources(slot, 1, &resource_view);
			break;
		case L'p':
			mOrigContext->PSSetShaderResources(slot, 1, &resource_view);
			break;
		case L'c':
			mOrigContext->CSSetShaderResources(slot, 1, &resource_view);
			break;
		default:
			// Should not happen
			return;
		}
		break;

	// TODO: case ResourceCopyTargetType::SAMPLER: // Not an ID3D11Resource, need to think about this one
	// TODO: 	break;
	// TODO: case ResourceCopyTargetType::VERTEX_BUFFER:
	// TODO: 	break;
	// TODO: case ResourceCopyTargetType::INDEX_BUFFER:
	// TODO: 	break;
	// TODO: case ResourceCopyTargetType::STREAM_OUTPUT:
	// TODO: 	break;

	case ResourceCopyTargetType::RENDER_TARGET:
	case ResourceCopyTargetType::DEPTH_STENCIL_TARGET:
		// XXX: HERE BE UNTESTED CODE PATHS!
		mOrigContext->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, render_view, &depth_view);
		if (type == ResourceCopyTargetType::RENDER_TARGET) {
			if (render_view[slot])
				render_view[slot]->Release();
			render_view[slot] = (ID3D11RenderTargetView*)view;
		} else {
			if (depth_view)
				depth_view->Release();
			depth_view = (ID3D11DepthStencilView*)view;
		}
		mOrigContext->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, render_view, depth_view);

		for (i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
			if (render_view[i])
				render_view[i]->Release();
		}
		depth_view->Release();
		break;

	// TODO: case ResourceCopyTargetType::UNORDERED_ACCESS_VIEW:
	// TODO: 	break;
	// TODO: case ResourceCopyTargetType::CUSTOM_RESOURCE:
	// TODO: 	break;
	}
}

D3D11_BIND_FLAG ResourceCopyTarget::BindFlags()
{
	switch(type) {
		case ResourceCopyTargetType::CONSTANT_BUFFER:
			return D3D11_BIND_CONSTANT_BUFFER;
		case ResourceCopyTargetType::SHADER_RESOURCE:
			return D3D11_BIND_SHADER_RESOURCE;
		// TODO: case ResourceCopyTargetType::VERTEX_BUFFER:
		// TODO: 	return D3D11_BIND_VERTEX_BUFFER;
		// TODO: case ResourceCopyTargetType::INDEX_BUFFER:
		// TODO: 	return D3D11_BIND_INDEX_BUFFER;
		// TODO: case ResourceCopyTargetType::STREAM_OUTPUT:
		// TODO: 	return D3D11_BIND_STREAM_OUTPUT;
		case ResourceCopyTargetType::RENDER_TARGET:
			return D3D11_BIND_RENDER_TARGET;
		case ResourceCopyTargetType::DEPTH_STENCIL_TARGET:
			return D3D11_BIND_DEPTH_STENCIL;
		// TODO: case ResourceCopyTargetType::UNORDERED_ACCESS_VIEW:
		// TODO: 	return D3D11_BIND_UNORDERED_ACCESS;
	}

	// Shouldn't happen. No return value makes sense, so raise an exception
	throw(std::range_error("Bad 3DMigoto ResourceCopyTarget"));
}

static ID3D11Buffer *CreateCompatibleBuffer(
		ID3D11Buffer *src_resource,
		D3D11_BIND_FLAG bind_flags,
		ID3D11Device *device)
{
	HRESULT hr;
	D3D11_BUFFER_DESC desc;
	ID3D11Buffer *buffer = NULL;

	src_resource->GetDesc(&desc);
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = bind_flags;
	desc.CPUAccessFlags = 0;
	// XXX: Any changes needed in desc.MiscFlags?

	hr = device->CreateBuffer(&desc, NULL, &buffer);
	if (FAILED(hr)) {
		LogInfo("Resource copy CreateCompatibleBuffer failed: 0x%x\n", hr);
		return NULL;
	}

	return buffer;
}

template <typename ResourceType,
	 typename DescType,
	HRESULT (__stdcall ID3D11Device::*CreateTexture)(THIS_
	      const DescType *pDesc,
	      const D3D11_SUBRESOURCE_DATA *pInitialData,
	      ResourceType **ppTexture)
	>
static ResourceType* CreateCompatibleTexture(
		ResourceType *src_resource,
		D3D11_BIND_FLAG bind_flags,
		DXGI_FORMAT format,
		ID3D11Device *device)
{
	HRESULT hr;
	DescType desc;
	ResourceType *tex = NULL;

	src_resource->GetDesc(&desc);
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = bind_flags;
	desc.CPUAccessFlags = 0;
	if (format)
		desc.Format = format;
	// XXX: Any changes needed in desc.MiscFlags?

	hr = (device->*CreateTexture)(&desc, NULL, &tex);
	if (FAILED(hr)) {
		LogInfo("Resource copy CreateCompatibleTexture failed: 0x%x\n", hr);
		return NULL;
	}

	return tex;
}

static DXGI_FORMAT GetViewFormat(ResourceCopyTargetType type, ID3D11View *view)
{
	ID3D11ShaderResourceView *resource_view = NULL;
	ID3D11RenderTargetView *render_view = NULL;
	ID3D11DepthStencilView *depth_view = NULL;

	D3D11_SHADER_RESOURCE_VIEW_DESC resource_view_desc;
	D3D11_RENDER_TARGET_VIEW_DESC render_view_desc;
	D3D11_DEPTH_STENCIL_VIEW_DESC depth_view_desc;

	if (!view)
		return DXGI_FORMAT_UNKNOWN;

	switch (type) {
		case ResourceCopyTargetType::SHADER_RESOURCE:
			resource_view = (ID3D11ShaderResourceView*)view;
			resource_view->GetDesc(&resource_view_desc);
			return resource_view_desc.Format;
		case ResourceCopyTargetType::RENDER_TARGET:
			render_view = (ID3D11RenderTargetView*)view;
			render_view->GetDesc(&render_view_desc);
			return render_view_desc.Format;
		case ResourceCopyTargetType::DEPTH_STENCIL_TARGET:
			depth_view = (ID3D11DepthStencilView*)view;
			depth_view->GetDesc(&depth_view_desc);
			return depth_view_desc.Format;
		// TODO: ResourceCopyTargetType::UNORDERED_ACCESS_VIEW:
	}

	return DXGI_FORMAT_UNKNOWN;
}

static ID3D11Resource* CreateCompatibleResource(
		ResourceCopyTarget *src,
		ResourceCopyTarget *dst,
		ID3D11Resource *src_resource,
		ID3D11View *src_view,
		ID3D11Device *device)
{
	D3D11_RESOURCE_DIMENSION dimension;
	D3D11_BIND_FLAG bind_flags = dst->BindFlags();
	DXGI_FORMAT format = GetViewFormat(src->type, src_view);
	ID3D11Resource *res = NULL;

	src_resource->GetType(&dimension);
	switch (dimension) {
		case D3D11_RESOURCE_DIMENSION_UNKNOWN:
			return NULL;
		case D3D11_RESOURCE_DIMENSION_BUFFER:
			return CreateCompatibleBuffer((ID3D11Buffer*)src_resource, bind_flags, device);
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			return CreateCompatibleTexture<ID3D11Texture1D, D3D11_TEXTURE1D_DESC, &ID3D11Device::CreateTexture1D>
				((ID3D11Texture1D*)src_resource, bind_flags, format, device);
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			return CreateCompatibleTexture<ID3D11Texture2D, D3D11_TEXTURE2D_DESC, &ID3D11Device::CreateTexture2D>
				((ID3D11Texture2D*)src_resource, bind_flags, format, device);
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			return CreateCompatibleTexture<ID3D11Texture3D, D3D11_TEXTURE3D_DESC, &ID3D11Device::CreateTexture3D>
				((ID3D11Texture3D*)src_resource, bind_flags, format, device);
	}

	return NULL;
}

static ID3D11View* CreateCompatibleView(ResourceCopyTarget *dst,
		ID3D11Resource *resource, ID3D11Device *device)
{
	ID3D11ShaderResourceView *resource_view = NULL;
	ID3D11RenderTargetView *render_view = NULL;
	ID3D11DepthStencilView *depth_view = NULL;
	HRESULT hr;

	// For now just creating a view of the whole resource to simplify
	// things. If/when we find a game that needs a view of a subresource we
	// can think about handling that case then.

	switch (dst->type) {
		case ResourceCopyTargetType::SHADER_RESOURCE:
			hr = device->CreateShaderResourceView(resource, NULL, &resource_view);
			if (SUCCEEDED(hr))
				return resource_view;
			LogInfo("Resource copy CreateCompatibleView failed for shader resource view: 0x%x\n", hr);
			break;
		case ResourceCopyTargetType::RENDER_TARGET:
			hr = device->CreateRenderTargetView(resource, NULL, &render_view);
			if (SUCCEEDED(hr))
				return render_view;
			LogInfo("Resource copy CreateCompatibleView failed for render target view: 0x%x\n", hr);
			break;
		case ResourceCopyTargetType::DEPTH_STENCIL_TARGET:
			hr = device->CreateDepthStencilView(resource, NULL, &depth_view);
			if (SUCCEEDED(hr))
				return depth_view;
			LogInfo("Resource copy CreateCompatibleView failed for depth/stencil view: 0x%x\n", hr);
			break;
		// TODO: ResourceCopyTargetType::UNORDERED_ACCESS_VIEW:
	}
	return NULL;
}

void ResourceCopyOperation::run(HackerContext *mHackerContext, ID3D11Device *mOrigDevice,
		ID3D11DeviceContext *mOrigContext, ShaderOverrideState *state)
{
	ID3D11Resource *src_resource = NULL;
	ID3D11Resource *dst_resource = NULL;
	ID3D11View *src_view = NULL;
	ID3D11View *dst_view = NULL;

	bool release_resource = false;
	bool release_view = false;

	if (src.type == ResourceCopyTargetType::EMPTY) {
		dst.SetResource(mOrigContext, NULL, NULL);
		return;
	}

	src_resource = src.GetResource(mOrigContext, &src_view);
	if (!src_resource) {
		LogDebug("Resource copy error: Source was NULL\n");
		return;
	}

	if (copy_type == ResourceCopyOperationType::COPY) {
		// TODO: if (!ResourceIsCompatible(src_resource, cached_resource) {
		// TODO:	 if (cached_resource)
		// TODO:		cached_resource->Release();
		// TODO:	 if (cached_view)
		// TODO:		cached_view->Release();
		dst_resource = CreateCompatibleResource(&src, &dst, src_resource, src_view, mOrigDevice);
		if (!dst_resource) {
			LogInfo("Resource copy error: Could not create destination resource\n");
			goto out_release;
		}
		release_resource = true; // TODO: Not once we cache it

		mOrigContext->CopyResource(dst_resource, src_resource);
		// TODO: cached_resource = dst_resource;
	} else {
		dst_resource = src_resource;
		if (src_view && (src.type == dst.type))
			dst_view = src_view;
	}

	if (!dst_view) {
		dst_view = CreateCompatibleView(&dst, dst_resource, mOrigDevice);
		// Not checking for NULL return as view's are not applicable to
		// all types. TODO: Check for legitimate failures.
		release_view = true; // TODO: Not once we cache it
		// TODO: cached_view = dst_view;
	}

	dst.SetResource(mOrigContext, dst_resource, dst_view);

out_release:
	if (release_resource && dst_resource)
		dst_resource->Release();

	if (release_view && dst_view)
		dst_view->Release();

	src_resource->Release();
	if (src_view)
		src_view->Release();
}
