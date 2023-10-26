#pragma once
/* Handles are obfuscated pointers whose pointed resoureces cannot be accessed
by the Handles themselves alone.
* Instead, the ownership, creation & destruction is 'handled' by their respective owners
*/
typedef size_t handle_type;
constexpr handle_type invalid_handle = -1;