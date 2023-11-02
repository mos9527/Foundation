#pragma once
#include "../../pch.hpp"
#include "../../Common/Handle.hpp"
namespace RHI {
	typedef std::basic_string<wchar_t> name_t;
	class RHIObject {
	protected:
		name_t m_Name;
	public:
		virtual name_t const& GetName() { return m_Name; }
		virtual void SetName(name_t) = 0;
	};
}