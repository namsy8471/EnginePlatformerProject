#pragma once

#define ThrowIfFailed(hr){ if (FAILED(hr)) { throw std::runtime_error("DX12 Error at Line " + std::to_string(__LINE__)); } }