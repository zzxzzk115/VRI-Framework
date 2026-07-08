#include "gf/io/ply.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gf/core/errors.h"
#include "gf/core/gauss_ir.h"
#include "gf/core/metadata.h"
#include "gf/core/validate.h"
#include "gf/io/reader.h"

namespace gf {

namespace {

int degreeForDim(int dim) {
  if (dim < 3)
    return 0;
  if (dim < 8)
    return 1;
  if (dim < 15)
    return 2;
  return 3;
}

// Read a line from memory buffer (skip comments)
bool getlineSkipComment(const uint8_t *&data, size_t &remaining,
                        std::string &line) {
  while (remaining > 0) {
    // Find line end
    size_t lineEnd = 0;
    bool found = false;
    for (size_t i = 0; i < remaining; ++i) {
      if (data[i] == '\n') {
        lineEnd = i;
        found = true;
        break;
      }
    }
    if (!found) {
      // No newline found, use all remaining data
      lineEnd = remaining;
    }

    // Extract line content
    line.assign(reinterpret_cast<const char *>(data), lineEnd);
    data += lineEnd + (found ? 1 : 0);
    remaining -= lineEnd + (found ? 1 : 0);

    // Remove leading whitespace
    auto start = line.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
      line.clear();
      continue;
    }
    std::string trimmed = line.substr(start);
    // Skip comment lines
    if (trimmed.rfind("comment", 0) == 0) {
      line.clear();
      continue;
    }
    line = trimmed;
    return true;
  }
  return false;
}

enum class PlyScalarType {
  Int8,
  UInt8,
  Int16,
  UInt16,
  Int32,
  UInt32,
  Float32,
  Float64,
};

struct PlyProperty {
  std::string name;
  PlyScalarType type = PlyScalarType::Float32;
  size_t size = sizeof(float);
};

bool parsePlyScalarType(const std::string &name, PlyScalarType &type,
                        size_t &size) {
  if (name == "char" || name == "int8") {
    type = PlyScalarType::Int8;
    size = sizeof(int8_t);
    return true;
  }
  if (name == "uchar" || name == "uint8" || name == "uint8_t") {
    type = PlyScalarType::UInt8;
    size = sizeof(uint8_t);
    return true;
  }
  if (name == "short" || name == "int16") {
    type = PlyScalarType::Int16;
    size = sizeof(int16_t);
    return true;
  }
  if (name == "ushort" || name == "uint16") {
    type = PlyScalarType::UInt16;
    size = sizeof(uint16_t);
    return true;
  }
  if (name == "int" || name == "int32") {
    type = PlyScalarType::Int32;
    size = sizeof(int32_t);
    return true;
  }
  if (name == "uint" || name == "uint32") {
    type = PlyScalarType::UInt32;
    size = sizeof(uint32_t);
    return true;
  }
  if (name == "float" || name == "float32") {
    type = PlyScalarType::Float32;
    size = sizeof(float);
    return true;
  }
  if (name == "double" || name == "float64") {
    type = PlyScalarType::Float64;
    size = sizeof(double);
    return true;
  }
  return false;
}

template <typename T> T readScalarLE(const uint8_t *data) {
  T value{};
  std::memcpy(&value, data, sizeof(T));
  return value;
}

float readPlyScalar(const uint8_t *data, PlyScalarType type) {
  switch (type) {
  case PlyScalarType::Int8:
    return static_cast<float>(readScalarLE<int8_t>(data));
  case PlyScalarType::UInt8:
    return static_cast<float>(readScalarLE<uint8_t>(data));
  case PlyScalarType::Int16:
    return static_cast<float>(readScalarLE<int16_t>(data));
  case PlyScalarType::UInt16:
    return static_cast<float>(readScalarLE<uint16_t>(data));
  case PlyScalarType::Int32:
    return static_cast<float>(readScalarLE<int32_t>(data));
  case PlyScalarType::UInt32:
    return static_cast<float>(readScalarLE<uint32_t>(data));
  case PlyScalarType::Float32:
    return readScalarLE<float>(data);
  case PlyScalarType::Float64:
    return static_cast<float>(readScalarLE<double>(data));
  }
  return 0.0f;
}

} // namespace

class PlyReader : public IGaussReader {
public:
  Expected<GaussianCloudIR> Read(const uint8_t *data, size_t size,
                                 const ReadOptions &options) override {
    try {
      if (data == nullptr || size == 0) {
        return Expected<GaussianCloudIR>(
            MakeError("ply read failed: empty input"));
      }

      const uint8_t *current = data;
      size_t remaining = size;

      std::string line;
      if (!getlineSkipComment(current, remaining, line) || line != "ply") {
        return Expected<GaussianCloudIR>(MakeError("ply read failed: not ply"));
      }
      if (!getlineSkipComment(current, remaining, line) ||
          line != "format binary_little_endian 1.0") {
        return Expected<GaussianCloudIR>(
            MakeError("ply read failed: unsupported format"));
      }
      if (!getlineSkipComment(current, remaining, line) ||
          line.find("element vertex ") != 0) {
        return Expected<GaussianCloudIR>(
            MakeError("ply read failed: missing vertex count"));
      }
      int numPoints = std::stoi(line.substr(std::strlen("element vertex ")));
      if (numPoints <= 0) {
        return Expected<GaussianCloudIR>(
            MakeError("ply read failed: invalid vertex count"));
      }

      // Keep every vertex property, not only the standard 3DGS float fields.
      // Extra columns such as importance, lod_level or cluster_id are forwarded to
      // libvasset so the engine can build an Ordered CLOD ranking after import.
      std::vector<PlyProperty> properties;
      std::unordered_map<std::string, int> fields;
      size_t rowStrideBytes = 0;
      for (;;) {
        if (!getlineSkipComment(current, remaining, line)) {
          return Expected<GaussianCloudIR>(
              MakeError("ply read failed: EOF in header"));
        }
        if (line == "end_header")
          break;
        const std::string prefix = "property ";
        if (line.rfind(prefix, 0) != 0) {
          return Expected<GaussianCloudIR>(
              MakeError("ply read failed: unsupported header line"));
        }

        std::istringstream propStream(line.substr(prefix.size()));
        std::string typeName;
        std::string name;
        propStream >> typeName >> name;
        if (typeName == "list" || name.empty()) {
          return Expected<GaussianCloudIR>(
              MakeError("ply read failed: unsupported property type"));
        }

        PlyProperty prop;
        prop.name = name;
        if (!parsePlyScalarType(typeName, prop.type, prop.size)) {
          return Expected<GaussianCloudIR>(
              MakeError("ply read failed: unsupported property type"));
        }

        fields[prop.name] = static_cast<int>(properties.size());
        rowStrideBytes += prop.size;
        properties.push_back(std::move(prop));
      }

      auto idx = [&fields](const std::string &name) -> int {
        auto it = fields.find(name);
        return it == fields.end() ? -1 : it->second;
      };

      std::vector<int> posIdx = {idx("x"), idx("y"), idx("z")};
      std::vector<int> scaleIdx = {idx("scale_0"), idx("scale_1"),
                                   idx("scale_2")};
      std::vector<int> rotIdx = {idx("rot_0"), idx("rot_1"), idx("rot_2"),
                                 idx("rot_3")};
      int alphaIdx = idx("opacity");
      std::vector<int> colorIdx = {idx("f_dc_0"), idx("f_dc_1"), idx("f_dc_2")};

      for (int v : posIdx)
        if (v < 0)
          return Expected<GaussianCloudIR>(
              MakeError("missing position fields"));
      for (int v : scaleIdx)
        if (v < 0)
          return Expected<GaussianCloudIR>(MakeError("missing scale fields"));
      for (int v : rotIdx)
        if (v < 0)
          return Expected<GaussianCloudIR>(MakeError("missing rot fields"));
      for (int v : colorIdx)
        if (v < 0)
          return Expected<GaussianCloudIR>(MakeError("missing color fields"));
      if (alphaIdx < 0)
        return Expected<GaussianCloudIR>(MakeError("missing opacity field"));

      std::vector<int> shIdx;
      for (int i = 0;; ++i) {
        int v = idx("f_rest_" + std::to_string(i));
        if (v < 0)
          break;
        shIdx.push_back(v);
      }
      int shDim = static_cast<int>(shIdx.size() / 3);

      // Read binary data
      const size_t dataSize = static_cast<size_t>(numPoints) * rowStrideBytes;
      if (remaining < dataSize) {
        return Expected<GaussianCloudIR>(
            MakeError("ply read failed: insufficient data"));
      }

      const size_t numValues =
          static_cast<size_t>(numPoints) * properties.size();
      std::vector<float> values(numValues);

      const uint8_t *row = current;
      for (int i = 0; i < numPoints; ++i) {
        const uint8_t *fieldData = row;
        float *dst = values.data() + static_cast<size_t>(i) * properties.size();
        for (size_t p = 0; p < properties.size(); ++p) {
          dst[p] = readPlyScalar(fieldData, properties[p].type);
          fieldData += properties[p].size;
        }
        row += rowStrideBytes;
      }
      current += dataSize;
      remaining -= dataSize;

      GaussianCloudIR ir;
      ir.numPoints = numPoints;
      ir.meta.shDegree = degreeForDim(shDim);
      ir.meta.sourceFormat = "ply";

      // Pre-allocate all vectors to avoid reallocation overhead
      // This helps compiler optimize memory access patterns
      ir.positions.resize(numPoints * 3);
      ir.scales.resize(numPoints * 3);
      ir.rotations.resize(numPoints * 4);
      ir.alphas.resize(numPoints);
      ir.colors.resize(numPoints * 3);
      ir.sh.resize(numPoints * shDim * 3);

      // Use pointers for direct writes to enable better compiler optimization
      // __restrict__ hints compiler that pointers don't alias
      float *__restrict__ posPtr = ir.positions.data();
      float *__restrict__ scalePtr = ir.scales.data();
      float *__restrict__ rotPtr = ir.rotations.data();
      float *__restrict__ alphaPtr = ir.alphas.data();
      float *__restrict__ colorPtr = ir.colors.data();
      float *__restrict__ shPtr = ir.sh.data();

      const float *__restrict__ valuesPtr = values.data();
      const size_t stride = properties.size();

      // Cache field indices in local variables for better register allocation
      const int pos0 = posIdx[0], pos1 = posIdx[1], pos2 = posIdx[2];
      const int scale0 = scaleIdx[0], scale1 = scaleIdx[1],
                scale2 = scaleIdx[2];
      const int rot0 = rotIdx[0], rot1 = rotIdx[1], rot2 = rotIdx[2],
                rot3 = rotIdx[3];
      const int color0 = colorIdx[0], color1 = colorIdx[1],
                color2 = colorIdx[2];

      auto isBaseFieldIndex = [&](int propIndex) {
        for (int v : posIdx)
          if (v == propIndex)
            return true;
        for (int v : scaleIdx)
          if (v == propIndex)
            return true;
        for (int v : rotIdx)
          if (v == propIndex)
            return true;
        for (int v : colorIdx)
          if (v == propIndex)
            return true;
        if (alphaIdx == propIndex)
          return true;
        for (int v : shIdx)
          if (v == propIndex)
            return true;
        return false;
      };

      std::vector<int> extraIdx;
      extraIdx.reserve(properties.size());
      for (size_t p = 0; p < properties.size(); ++p) {
        if (!isBaseFieldIndex(static_cast<int>(p))) {
          extraIdx.push_back(static_cast<int>(p));
          ir.extras[properties[p].name].resize(numPoints);
        }
      }

      // Optimized loop: direct writes with pointer arithmetic
      // Compiler can auto-vectorize this pattern easily
      for (int i = 0; i < numPoints; ++i) {
        const float *__restrict__ base =
            valuesPtr + static_cast<size_t>(i) * stride;

        // Write positions (3 floats) - contiguous writes enable SIMD
        posPtr[0] = base[pos0];
        posPtr[1] = base[pos1];
        posPtr[2] = base[pos2];
        posPtr += 3;

        // Write scales (3 floats)
        scalePtr[0] = base[scale0];
        scalePtr[1] = base[scale1];
        scalePtr[2] = base[scale2];
        scalePtr += 3;

        // Write rotations (4 floats)
        rotPtr[0] = base[rot0];
        rotPtr[1] = base[rot1];
        rotPtr[2] = base[rot2];
        rotPtr[3] = base[rot3];
        rotPtr += 4;

        // Write alpha (1 float)
        *alphaPtr++ = base[alphaIdx];

        // Write colors (3 floats)
        colorPtr[0] = base[color0];
        colorPtr[1] = base[color1];
        colorPtr[2] = base[color2];
        colorPtr += 3;

        // Store SH coefficients in interleaved RGB order per coefficient
        // Unroll inner loop for better optimization
        for (int j = 0; j < shDim; ++j) {
          shPtr[0] = base[shIdx[j]];             // coeff j, R
          shPtr[1] = base[shIdx[j + shDim]];     // coeff j, G
          shPtr[2] = base[shIdx[j + 2 * shDim]]; // coeff j, B
          shPtr += 3;
        }

        for (int propIndex : extraIdx) {
          ir.extras[properties[propIndex].name][i] = base[propIndex];
        }
      }

      const auto err = ValidateBasic(ir, options.strict);
      if (!err.message.empty() && options.strict)
        return Expected<GaussianCloudIR>(err);
      return Expected<GaussianCloudIR>(std::move(ir));
    } catch (const std::exception &e) {
      return Expected<GaussianCloudIR>(
          MakeError(std::string("ply read failed: ") + e.what()));
    }
  }
};

std::unique_ptr<IGaussReader> MakePlyReader() {
  return std::make_unique<PlyReader>();
}

} // namespace gf
