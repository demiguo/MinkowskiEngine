/*  Copyright (c) Chris Choy (chrischoy@ai.stanford.edu).
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy of
 *  this software and associated documentation files (the "Software"), to deal in
 *  the Software without restriction, including without limitation the rights to
 *  use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 *  of the Software, and to permit persons to whom the Software is furnished to do
 *  so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 *  Please cite "4D Spatio-Temporal ConvNets: Minkowski Convolutional Neural
 *  Networks", CVPR'19 (https://arxiv.org/abs/1904.08755) if you use any part
 *  of the code.
 */
#include "common.hpp"
#include "coords_hashmaps.hpp"
#include "coords_kernelmaps.hpp"
#include "region_iter.hpp"
#include "utils.hpp"

#include <pybind11/pybind11.h>

namespace py = pybind11;

template <uint8_t D, typename Itype> CoordsManager<D, Itype>::CoordsManager() {
  if (!CoordsManager<D, Itype>::pool) {
    CoordsManager(std::thread::hardware_concurrency());
  }
}

template <uint8_t D, typename Itype>
CoordsManager<D, Itype>::CoordsManager(int nthreads_) {
  if (!CoordsManager<D, Itype>::pool) {
    CoordsManager<D, Itype>::nthreads = nthreads_;
    std::cout << "Setting the ME of dimension " << int(D)
              << " number of threads to " << CoordsManager<D, Itype>::nthreads
              << std::endl;
    CoordsManager<D, Itype>::pool.reset(
        new CoordsThreadPool<D, Itype>(CoordsManager<D, Itype>::nthreads));
  }
}

/*
 * Given tensor_stride_src and tensor_stride_dst, find the respective coord_maps
 * and return the indices of the coord_map_ind in coord_map_dst
 */
template <uint8_t D, typename Itype>
void CoordsManager<D, Itype>::getCoordsMapping(at::Tensor mapping,
                                               py::object py_in_coords_key,
                                               py::object py_out_coords_key) {
  PyCoordsKey<D> *p_in_coords_key = py_in_coords_key.cast<PyCoordsKey<D> *>();
  PyCoordsKey<D> *p_out_coords_key = py_out_coords_key.cast<PyCoordsKey<D> *>();

  if (!p_in_coords_key->key_set || !p_out_coords_key->key_set)
    throw std::invalid_argument(Formatter() << "CoordsKey is not initialized.");

  uint64_t in_coords_key = p_in_coords_key->getKey(),
           out_coords_key = p_out_coords_key->getKey();
  if (coords_hashmaps.find(in_coords_key) == coords_hashmaps.end() ||
      coords_hashmaps.find(out_coords_key) == coords_hashmaps.end())
    throw std::invalid_argument(
        Formatter() << "CoordsManager::getPermutation: Cannot find the given "
                       "coords key. in_coords_key: "
                    << std::to_string(in_coords_key)
                    << ", out_coords_key: " << std::to_string(out_coords_key));

  auto in_tensor_strides = p_in_coords_key->getTensorStride();
  auto out_tensor_strides = p_out_coords_key->getTensorStride();
  auto strides = std::vector<Itype>(D);

  for (int i = 0; i < D; i++) {
    strides[i] = out_tensor_strides[i] / in_tensor_strides[i];
    if (in_tensor_strides[i] > out_tensor_strides[i]) {
      throw std::invalid_argument(
          Formatter() << "Out tensor stride must be greater than the "
                         "in tensor stride. in_tensor_strides: "
                      << ArrToString<Arr<D, int>>(in_tensor_strides)
                      << ", out_tensor_strides: "
                      << ArrToString<Arr<D, int>>(out_tensor_strides));
    }
  }

  int in_ind, out_ind, nrows = getCoordsSize(py_in_coords_key);
  mapping.resize_({nrows, 1});
  mapping.fill_(-1);

  _CoordsHashMap<D, Itype> &in_coords = coords_hashmaps[in_coords_key].map;
  _CoordsHashMap<D, Itype> &out_coords = coords_hashmaps[out_coords_key].map;

  for (const auto &pair : in_coords) {
    Coord<D, Itype> coord = pair.first;
    in_ind = pair.second;
    for (int i = 0; i < D; i++) {
      coord[i] = int(floor(((float)coord[i]) / strides[i])) * strides[i];
    }
    out_ind = out_coords[coord];
    mapping[in_ind] = out_ind;
  }
}

/*
 * Given tensor_stride_src and tensor_stride_dst, find the respective coord_maps
 * and return the indices of the coord_map_ind in coord_map_dst
 */
template <uint8_t D, typename Itype>
void CoordsManager<D, Itype>::getKernelMap(
    at::Tensor kernel_map, std::vector<int> tensor_strides,
    std::vector<int> strides, std::vector<int> kernel_sizes,
    std::vector<int> dilations, int region_type, py::object py_in_coords_key,
    py::object py_out_coords_key, bool is_transpose) {
  InOutMapKey map_key = getMapHashKey(tensor_strides, strides, kernel_sizes,
                                      dilations, region_type, py_in_coords_key,
                                      py_out_coords_key, is_transpose);

  if (in_maps.find(map_key) == in_maps.end()) {
    throw std::invalid_argument(Formatter() << "The kernelmap does not exist.");
  }

  const InOutMapPerKernel<Itype> &in_map = in_maps[map_key];
  const InOutMapPerKernel<Itype> &out_map = out_maps[map_key];

  int all_volume = 0, kernel_volume = in_map.size();
  for (int k = 0; k < kernel_volume; k++)
    all_volume += in_map[k].size();

  kernel_map.resize_({all_volume, 3});
  Itype *p_kernel_map = kernel_map.data<Itype>();

  int curr_counter = 0;
  for (int k = 0; k < kernel_volume; k++) {
    int curr_volume = in_map[k].size();
    for (int i = 0; i < curr_volume; i++) {
      p_kernel_map[3 * curr_counter + 0] = k;
      p_kernel_map[3 * curr_counter + 1] = in_map[k][i];
      p_kernel_map[3 * curr_counter + 2] = out_map[k][i];
      curr_counter++;
    }
  }
}

template <uint8_t D, typename Itype>
uint64_t
CoordsManager<D, Itype>::getCoordsKey(const Arr<D, int> &tensor_strides) {
  auto tensor_stride_hash = hash_vec<Arr<D, int>>(tensor_strides);
  uint64_t r_coords_key = 0;

  // Following lines are from INITIALIZE_IN_COORDS
  /* Prioritize the p_coords_key */
  if (coords_hashmaps.find(tensor_stride_hash) != coords_hashmaps.end()) {
    r_coords_key = tensor_stride_hash;
  } else {
    throw std::invalid_argument(
        Formatter()
        << "The coord map doesn't exist for the given tensor strides "
        << "tensor_stride: " << ArrToString(tensor_strides) << " at "
        << __FILE__ << ":" << __LINE__);
  }

  return r_coords_key;
}

template <uint8_t D, typename Itype>
bool CoordsManager<D, Itype>::existsCoordsKey(uint64_t coords_key) {
  bool exist = false;
  // Following lines are from INITIALIZE_IN_COORDS
  /* Prioritize the p_coords_key */
  if (coords_hashmaps.find(coords_key) != coords_hashmaps.end())
    exist = true;
  return exist;
}

template <uint8_t D, typename Itype>
bool CoordsManager<D, Itype>::existsCoordsKey(py::object py_coords_key) {
  PyCoordsKey<D> *p_coords_key = py_coords_key.cast<PyCoordsKey<D> *>();
  return existsCoordsKey(p_coords_key->getKey());
}

template <uint8_t D, typename Itype>
int CoordsManager<D, Itype>::getCoordsSize(uint64_t coords_key) {
  if (!existsCoordsKey(coords_key))
    throw std::invalid_argument(
        Formatter() << "The coord map doesn't exist for the given coords_key "
                    << "coords_key: " << coords_key << " at " << __FILE__ << ":"
                    << __LINE__);
  return coords_hashmaps[coords_key].size();
}

template <uint8_t D, typename Itype>
int CoordsManager<D, Itype>::getCoordsSize(py::object py_coords_key) {
  PyCoordsKey<D> *p_coords_key = py_coords_key.cast<PyCoordsKey<D> *>();
  return getCoordsSize(p_coords_key->getKey());
}

template <uint8_t D, typename Itype>
void CoordsManager<D, Itype>::getCoords(at::Tensor coords,
                                        py::object py_coords_key) {
  PyCoordsKey<D> *p_coords_key = py_coords_key.cast<PyCoordsKey<D> *>();
  uint64_t coords_key = p_coords_key->getKey();
  if (!existsCoordsKey(coords_key))
    throw std::invalid_argument(
        Formatter() << "The coord map doesn't exist for the given coords_key "
                    << "coords_key: " << coords_key << " at " << __FILE__ << ":"
                    << __LINE__);
  int nrows = getCoordsSize(coords_key);
  coords.resize_({nrows, D + 1});
  Itype *p_coords = coords.data<Itype>();
  int ncols = D + 1;
  for (const auto &i : coords_hashmaps[coords_key].map) {
    Coord<D, Itype> coord(i.first);
    int n = i.second;
    std::copy(coord.data(), coord.data() + ncols, &p_coords[n * ncols]);
  }
}

/*******************************
 * Initialization
 *******************************/
template <uint8_t D, typename Itype>
uint64_t
CoordsManager<D, Itype>::initializeCoords(at::Tensor coords,
                                          const Arr<D, int> &tensor_strides,
                                          bool enforce_creation) {
  uint64_t key = hash_vec<Arr<D, int>>(tensor_strides);
  bool key_exists = coords_hashmaps.find(key) != coords_hashmaps.end();
  if (key_exists) {
    if (!enforce_creation)
      throw std::invalid_argument(
          Formatter()
          << "The coord map already exists for the given tensor stride "
          << "tensor_stride: " << ArrToString(tensor_strides) << " at "
          << __FILE__ << ":" << __LINE__);
    else {
      key = random();
      while (coords_hashmaps.find(key) != coords_hashmaps.end())
        key = random();
    }
  } // If key doesn't exist, use the current key regardless of enforce creation
  coords_hashmaps[key] = createCoordsHashMap(coords);
  return key;
}

template <uint8_t D, typename Itype>
uint64_t CoordsManager<D, Itype>::initializeCoords(at::Tensor coords,
                                                   py::object py_coords_key,
                                                   bool enforce_creation) {
  PyCoordsKey<D> *p_coords_key = py_coords_key.cast<PyCoordsKey<D> *>();
  uint64_t in_coords_key =
      initializeCoords(coords, p_coords_key->tensor_strides_, enforce_creation);
  p_coords_key->setKey(in_coords_key);
  return in_coords_key;
}

/********************************
 */
template <uint8_t D, typename Itype>
uint64_t CoordsManager<D, Itype>::createOutCoords(
    uint64_t coords_key, const Arr<D, int> &tensor_strides,
    const Arr<D, int> &strides, bool is_transpose) {
  if (!existsCoordsKey(coords_key))
    throw std::invalid_argument(
        Formatter() << "The coord map doesn't exist for the given coords_key. "
                    << "coords_key: " << coords_key << " at " << __FILE__ << ":"
                    << __LINE__);

  auto out_tensor_strides =
      computeOutTensorStride<D>(tensor_strides, strides, is_transpose);
  uint64_t out_coords_key = hash_vec<std::vector<int>>(out_tensor_strides);
  // If the coordinates already exists, return the key.
  if (existsCoordsKey(out_coords_key))
    return out_coords_key;

  coords_hashmaps[out_coords_key] =
      createOutCoordsHashMap(coords_key, tensor_strides, strides);
  return out_coords_key;
}

template <uint8_t D, typename Itype>
uint64_t
CoordsManager<D, Itype>::createPruneCoords(at::Tensor use_feat,
                                           py::object py_in_coords_key,
                                           py::object py_out_coords_key) {
  PyCoordsKey<D> *p_in_coords_key = py_in_coords_key.cast<PyCoordsKey<D> *>();
  PyCoordsKey<D> *p_out_coords_key = py_out_coords_key.cast<PyCoordsKey<D> *>();
  // set the coords key
  uint64_t out_coords_key = random();
  while (coords_hashmaps.find(out_coords_key) != coords_hashmaps.end())
    out_coords_key = random();
  // Set the pycoordskey
  p_out_coords_key->setTensorStride(p_in_coords_key->tensor_strides_);
  p_out_coords_key->setKey(out_coords_key);
  // Create coords hashmap
  coords_hashmaps[out_coords_key] =
      createPrunedCoordsHashMap(p_in_coords_key->getKey(), use_feat);
  return out_coords_key;
}

template <uint8_t D, typename Itype>
uint64_t CoordsManager<D, Itype>::createOriginCoords(uint64_t coords_key,
                                                     int batch_size) {
  if (!existsCoordsKey(coords_key))
    throw std::invalid_argument(
        Formatter() << "The coord map doesn't exist for the given coords_key. "
                    << "coords_key: " << coords_key << " at " << __FILE__ << ":"
                    << __LINE__);
  Arr<D, int> zero_tensor_strides;
  zero_tensor_strides.fill(0);
  uint64_t out_coords_key = hash_vec<Arr<D, int>>(zero_tensor_strides);
  // If the coordinates already exists, return the key.
  if (existsCoordsKey(out_coords_key))
    return out_coords_key;

  coords_hashmaps[out_coords_key] =
      createOriginCoordsHashMap(coords_key, batch_size);
  return out_coords_key;
}

template <uint8_t D, typename Itype>
InOutMapKey CoordsManager<D, Itype>::getMapHashKey(
    std::vector<int> tensor_strides, std::vector<int> strides,
    std::vector<int> kernel_sizes, std::vector<int> dilations, int region_type,
    py::object py_in_coords_key, py::object py_out_coords_key,
    bool is_transpose) {
  if (tensor_strides.size() != D || strides.size() != D ||
      kernel_sizes.size() != D || dilations.size() != D) {
    throw std::invalid_argument(
        Formatter() << "Size mismatch. tensor_strides: "
                    << ArrToString(tensor_strides)
                    << ", strides: " << ArrToString(strides)
                    << ", kernel_sizes: " << ArrToString(kernel_sizes)
                    << ", dilations: " << ArrToString(dilations));
  }
  Arr<D, int> arr_tensor_strides;
  Arr<D, int> arr_strides;
  Arr<D, int> arr_kernel_sizes;
  Arr<D, int> arr_dilations;
  std::copy_n(tensor_strides.begin(), D, arr_tensor_strides.begin());
  std::copy_n(strides.begin(), D, arr_strides.begin());
  std::copy_n(kernel_sizes.begin(), D, arr_kernel_sizes.begin());
  std::copy_n(dilations.begin(), D, arr_dilations.begin());
  return getMapHashKey(arr_tensor_strides, arr_strides, arr_kernel_sizes,
                       arr_dilations, region_type, py_in_coords_key,
                       py_out_coords_key, is_transpose);
}

template <uint8_t D, typename Itype>
InOutMapKey CoordsManager<D, Itype>::getMapHashKey(
    Arr<D, int> tensor_strides, Arr<D, int> strides, Arr<D, int> kernel_sizes,
    Arr<D, int> dilations, int region_type, py::object py_in_coords_key,
    py::object py_out_coords_key, bool is_transpose) {
  PyCoordsKey<D> *p_in_coords_key = py_in_coords_key.cast<PyCoordsKey<D> *>();
  PyCoordsKey<D> *p_out_coords_key = py_out_coords_key.cast<PyCoordsKey<D> *>();
  uint64_t out_coords_key = p_out_coords_key->getKey();
  uint64_t in_coords_key = p_in_coords_key->getKey();
  uint64_t stride_hash = hash_vec<Arr<D, int>>(strides);
  uint64_t kernel_size_hash = hash_vec<Arr<D, int>>(kernel_sizes);
  uint64_t dilation_hash = hash_vec<Arr<D, int>>(dilations);
  InOutMapKey map_key = {
      in_coords_key, out_coords_key,        stride_hash, kernel_size_hash,
      dilation_hash, (uint64_t)region_type, is_transpose};
  return map_key;
}

template <uint8_t D, typename Itype>
InOutMapKey
CoordsManager<D, Itype>::getOriginMapHashKey(py::object py_in_coords_key,
                                             py::object py_out_coords_key) {
  PyCoordsKey<D> *p_in_coords_key = py_in_coords_key.cast<PyCoordsKey<D> *>();
  PyCoordsKey<D> *p_out_coords_key = py_out_coords_key.cast<PyCoordsKey<D> *>();
  uint64_t out_coords_key = p_out_coords_key->getKey();
  uint64_t in_coords_key = p_in_coords_key->getKey();
  uint64_t zero_hash = hash_vec<Arr<D, int>>(Arr<D, int>());
  InOutMapKey map_key = {
      in_coords_key, out_coords_key, zero_hash, zero_hash, zero_hash, 0, false};
  return map_key;
}

template <uint8_t D, typename Itype>
InOutMapKey CoordsManager<D, Itype>::getOriginMapHashKeyCheck(
    py::object py_in_coords_key, py::object py_out_coords_key) {
  PyCoordsKey<D> *p_in_coords_key = py_in_coords_key.cast<PyCoordsKey<D> *>();
  PyCoordsKey<D> *p_out_coords_key = py_out_coords_key.cast<PyCoordsKey<D> *>();
  if (!p_in_coords_key->key_set or !p_out_coords_key->key_set)
    throw std::invalid_argument(Formatter()
                                << "Key is not set. in_coords_key: "
                                << std::to_string(p_in_coords_key->getKey())
                                << ", out_coords_key: "
                                << std::to_string(p_out_coords_key->getKey()));
  // Use the global pooling mapping
  uint64_t out_coords_key = p_out_coords_key->getKey();
  uint64_t in_coords_key = p_in_coords_key->getKey();
  uint64_t zero_hash = hash_vec<Arr<D, int>>(Arr<D, int>());
  InOutMapKey map_key = {
      in_coords_key, out_coords_key, zero_hash, zero_hash, zero_hash, 0, false};

  return map_key;
}

template <uint8_t D, typename Itype>
std::tuple<InOutMapPerKernel<Itype> &, InOutMapPerKernel<Itype> &>
CoordsManager<D, Itype>::setupAndReturnInOutPerKernel(
    std::vector<int> tensor_strides, std::vector<int> strides,
    std::vector<int> kernel_sizes, std::vector<int> dilations, int region_type,
    at::Tensor offsets, py::object py_in_coords_key,
    py::object py_out_coords_key, bool is_transpose) {
  if (tensor_strides.size() != D || strides.size() != D ||
      kernel_sizes.size() != D || dilations.size() != D) {
    throw std::invalid_argument(
        Formatter() << "Size mismatch. tensor_strides: "
                    << tensor_strides.size() << ", strides: " << strides.size()
                    << ", kernel_sizes: " << kernel_sizes.size()
                    << ", dilations: " << dilations.size());
  }
  Arr<D, int> arr_tensor_strides;
  Arr<D, int> arr_strides;
  Arr<D, int> arr_kernel_sizes;
  Arr<D, int> arr_dilations;
  std::copy_n(tensor_strides.begin(), D, arr_tensor_strides.begin());
  std::copy_n(strides.begin(), D, arr_strides.begin());
  std::copy_n(kernel_sizes.begin(), D, arr_kernel_sizes.begin());
  std::copy_n(dilations.begin(), D, arr_dilations.begin());
  return setupAndReturnInOutPerKernel(
      arr_tensor_strides, arr_strides, arr_kernel_sizes, arr_dilations,
      region_type, offsets, py_in_coords_key, py_out_coords_key, is_transpose);
}

/**
 * Setup output coords, set the py_out_coords_key, create in_out_kernel_map
 */
template <uint8_t D, typename Itype>
std::tuple<InOutMapPerKernel<Itype> &, InOutMapPerKernel<Itype> &>
CoordsManager<D, Itype>::setupAndReturnInOutPerKernel(
    Arr<D, int> tensor_strides, Arr<D, int> strides, Arr<D, int> kernel_sizes,
    Arr<D, int> dilations, int region_type, at::Tensor offsets,
    py::object py_in_coords_key, py::object py_out_coords_key,
    bool is_transpose) {
  PyCoordsKey<D> *p_in_coords_key = py_in_coords_key.cast<PyCoordsKey<D> *>();
  PyCoordsKey<D> *p_out_coords_key = py_out_coords_key.cast<PyCoordsKey<D> *>();
  uint64_t out_coords_key, in_coords_key = p_in_coords_key->getKey();

  // Create output coordinates if it doesn't exist
  if (!p_out_coords_key->key_set) {
    out_coords_key = createOutCoords(p_in_coords_key->getKey(), tensor_strides,
                                     strides, is_transpose);
    p_out_coords_key->setKey(out_coords_key);
  } else
    out_coords_key = p_out_coords_key->getKey();

  InOutMapKey map_key = getMapHashKey(tensor_strides, strides, kernel_sizes,
                                      dilations, region_type, py_in_coords_key,
                                      py_out_coords_key, is_transpose);

  if (!is_transpose) { // NON TRANSPOSE
    p_out_coords_key->setTensorStride(tensor_strides);
    p_out_coords_key->stride(strides);
    // For non transpose case
    // make a kernel mapping. The kernel will be saved with the map_key.
    if (in_maps.find(map_key) == in_maps.end()) {
      auto in_out = createInOutPerKernelInThreads(
          in_coords_key, out_coords_key, tensor_strides, kernel_sizes,
          dilations, region_type, offsets);
      in_maps[map_key] = std::get<0>(in_out);
      out_maps[map_key] = std::get<1>(in_out);
    }
    return std::make_tuple(std::ref(in_maps[map_key]),
                           std::ref(out_maps[map_key]));

  } else { // TRANSPOSE
    p_out_coords_key->setTensorStride(tensor_strides);
    p_out_coords_key->up_stride(strides);
    // Create temporary key for the flipped in/out
    InOutMapKey tmp_map_key =
        getMapHashKey(tensor_strides, strides, kernel_sizes, dilations,
                      region_type, py_out_coords_key, py_in_coords_key, false);
    // Check if the temporary key exists and return swapped in/out
    if (in_maps.find(tmp_map_key) != in_maps.end()) {
      return std::make_tuple(std::ref(out_maps[tmp_map_key]),
                             std::ref(in_maps[tmp_map_key]));
    } else {
      // create in out kernel if it doesn't exist
      auto out_tensor_strides = p_out_coords_key->getTensorStride();
      auto in_out = createInOutPerKernelTranspose(
          in_coords_key, out_coords_key, out_tensor_strides, kernel_sizes,
          dilations, region_type, offsets);
      in_maps[map_key] = std::get<0>(in_out);
      out_maps[map_key] = std::get<1>(in_out);

      return std::make_tuple(std::ref(in_maps[map_key]),
                             std::ref(out_maps[map_key]));
    }
  }
}

template <uint8_t D, typename Itype>
std::tuple<InOutMapPerKernel<Itype> &, InOutMapPerKernel<Itype> &>
CoordsManager<D, Itype>::setupAndReturnOriginInOutPerKernel(
    int batch_size, py::object py_in_coords_key, py::object py_out_coords_key) {
  PyCoordsKey<D> *p_in_coords_key = py_in_coords_key.cast<PyCoordsKey<D> *>();
  PyCoordsKey<D> *p_out_coords_key = py_out_coords_key.cast<PyCoordsKey<D> *>();
  uint64_t out_coords_key, in_coords_key = p_in_coords_key->getKey();

  // Create output coordinates if it doesn't exist
  if (!p_out_coords_key->key_set) {
    out_coords_key = createOriginCoords(p_in_coords_key->getKey(), batch_size);
    p_out_coords_key->setKey(out_coords_key);
    p_out_coords_key->setTensorStride(Arr<D, int>());
  } else
    out_coords_key = p_out_coords_key->getKey();

  // Map key for origin hash map
  InOutMapKey map_key =
      getOriginMapHashKey(py_in_coords_key, py_out_coords_key);
  // For non transpose case
  // make a kernel mapping. The kernel will be saved with the map_key.
  if (in_maps.find(map_key) == in_maps.end()) {
    auto in_out = createGlobalReductionInOutMap(in_coords_key, out_coords_key);
    in_maps[map_key] = std::get<0>(in_out);
    out_maps[map_key] = std::get<1>(in_out);
  }
  return std::make_tuple(std::ref(in_maps[map_key]),
                         std::ref(out_maps[map_key]));
}

template <uint8_t D, typename Itype>
std::tuple<InOutMapPerKernel<Itype> &, InOutMapPerKernel<Itype> &>
CoordsManager<D, Itype>::setupAndReturnPruningInOutPerKernel(
    at::Tensor use_feat, py::object py_in_coords_key,
    py::object py_out_coords_key) {
  PyCoordsKey<D> *p_in_coords_key = py_in_coords_key.cast<PyCoordsKey<D> *>();
  PyCoordsKey<D> *p_out_coords_key = py_out_coords_key.cast<PyCoordsKey<D> *>();
  uint64_t out_coords_key, in_coords_key = p_in_coords_key->getKey();

  // Create output coordinates if it doesn't exist
  if (!p_out_coords_key->key_set)
    // The following function setup py_out_coords_key
    out_coords_key =
        createPruneCoords(use_feat, py_in_coords_key, py_out_coords_key);
  else
    out_coords_key = p_out_coords_key->getKey();

  // Use the map key for origin hash map (stride, dilation, kernel are all NULL)
  InOutMapKey map_key =
      getOriginMapHashKey(py_in_coords_key, py_out_coords_key);

  // For non transpose case
  // make a kernel mapping. The kernel will be saved with the map_key.
  if (in_maps.find(map_key) == in_maps.end()) {
    auto in_out = createPruningInOutMap(in_coords_key, out_coords_key);
    in_maps[map_key] = std::get<0>(in_out);
    out_maps[map_key] = std::get<1>(in_out);
  }

  return std::make_tuple(std::ref(in_maps[map_key]),
                         std::ref(out_maps[map_key]));
}

template <uint8_t D, typename Itype>
std::string CoordsManager<D, Itype>::toString() const {
  std::string tmp;
  tmp += "< CoordsManager, Number of Coords: ";
  tmp += std::to_string(coords_hashmaps.size());
  for (auto kv : coords_hashmaps) {
    tmp += " \n\tCoords Key: ";
    tmp += std::to_string(kv.first);
    tmp += ", Size: ";
    tmp += std::to_string((kv.second).size());
  }
  tmp += "\n  Number of Kernel Maps: ";
  tmp += std::to_string(in_maps.size());
  for (auto kv : in_maps) {
    tmp += " \n\tKernel Map Key: ";
    tmp += std::to_string(hash_vec<InOutMapKey>(kv.first));
    int size = 0;
    for (auto map : kv.second) {
      size += map.size();
    }
    tmp += ", Size: ";
    tmp += std::to_string(size);
  }
  tmp += "\n  Number of threads: ";
  tmp += std::to_string(CoordsManager<D, Itype>::nthreads);
  tmp += " >";
  return tmp;
}

INSTANTIATE_CLASS_DIM_ITYPE(CoordsManager, int32_t);
