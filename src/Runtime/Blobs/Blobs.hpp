#pragma once
#include <cstdint>
#include <iostream>
#include <RHICore/Common.hpp>
#include <RHICore/Resource.hpp>
namespace Foundation::Blobs {
    using byte_t = char;
    using byte_span = Core::StlSpan<byte_t>;
    using const_byte_span = Core::StlSpan<const byte_t>;
    /// <summary>
    /// RAII stream interface.
    /// </summary>
    struct Stream {
        /// <summary>
        /// Reads data from the stream into the provided span.        
        /// </summary>        
        /// <returns>Number of bytes actually read.</returns>
        virtual size_t Read(byte_span span) = 0;
        /// <summary>
        /// Writes data from the provided span to the stream.
        /// </summary>
        /// <returns>Number of bytes actually written.</returns>
        virtual size_t Write(const_byte_span span) = 0;
        /// <summary>
        /// Moves the current position within a stream to a specified offset relative to a given origin.
        /// </summary>
        /// <param name="offset">The number of bytes to move the position by, relative to the origin.</param>
        /// <param name="origin">The reference point for the offset. Defaults to the beginning of the stream (std::ios_base::beg).</param>
        /// <returns>The new position within the stream, measured in bytes from the beginning (std::ios_base::beg).</returns>
        virtual size_t Seek(size_t offset, std::ios_base::seekdir origin = std::ios_base::beg) = 0;
        /// <summary>
        /// Returns the current position in the stream.
        /// </summary>
        /// <returns>
        /// The current position in the stream, typically as a byte offset from the beginning.
        /// Implementations without a bounded size may return any value as it's undefined.
        /// </returns>
        virtual size_t Tell() = 0;
        /// <summary>
        /// Flushes any buffered data, ensuring it is written or processed immediately.
        /// Implementations that do not support flushing should treat this as a no-op.
        /// </summary>
        virtual void Flush() = 0;
    };

    struct Blob {
        virtual ~Blob() = default;
        virtual void Serialize(Stream& stream) = 0;
        virtual void Deserialize(Stream& stream) = 0;
    };
}
