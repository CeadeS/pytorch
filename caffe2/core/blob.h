#ifndef CAFFE2_CORE_BLOB_H_
#define CAFFE2_CORE_BLOB_H_

#include <cstddef>
#include <sstream>
#include <typeinfo>
#include <type_traits>
#include <vector>

#include "caffe2/core/blob_serializer_base.h"
#include "caffe2/core/common.h"
#include "caffe2/core/logging.h"
#include "caffe2/core/tensor.h"
#include "caffe2/core/typeid.h"
#include "caffe2/proto/caffe2_pb.h"
#include <ATen/core/intrusive_ptr.h>

namespace caffe2 {

/**
 * @brief Blob is a general container that hosts a typed pointer.
 *
 * A Blob hosts a pointer as well as its type, and takes charge of deleting it
 * properly when the blob is deallocated or re-allocated with a new type. A blob
 * could contain anything, although the most common case is to contain a Tensor.
 */
class CAFFE2_API Blob final : public c10::intrusive_ptr_target {
 public:
  /**
   * Initializes an empty Blob.
   */
  Blob() noexcept : meta_(), pointer_(nullptr), has_ownership_(false) {}
  ~Blob() { Reset(); }

  Blob(Blob&& other) noexcept : Blob() {
    swap(other);
  }

  Blob& operator=(Blob&& other) noexcept {
    Blob(std::move(other)).swap(*this);
    return *this;
  }

  /**
   * Checks if the content stored in the blob is of type T.
   */
  template <class T>
  bool IsType() const noexcept {
    return meta_.Match<T>();
  }

  bool IsTensorType(DeviceType device_type) const {
    bool is_match = meta_.Match<Tensor>();
    auto* tensor = static_cast<Tensor*>(pointer_);
    if (is_match && tensor && tensor->GetDeviceType() == device_type) {
      return true;
    }
    return false;
  }

  /**
   * Returns the meta info of the blob.
   */
  inline const TypeMeta& meta() const noexcept { return meta_; }

  /**
   * Returns a printable typename of the blob.
   */
  inline const char* TypeName() const noexcept { return meta_.name(); }

  /**
   * @brief Gets the const reference of the stored object. The code checks if
   * the stored object is of the desired type.
   */
  // TODO(jerryzh): add a Get(DeviceType) function?
  template <class T>
  const T& Get() const {
    CAFFE_ENFORCE(
        IsType<T>(),
        "wrong type for the Blob instance. Blob contains ",
        meta_.name(),
        " while caller expects ",
        TypeMeta::TypeName<T>());
    // TODO: after we add Get<Tensor>(DeviceType)
    // and changed all the callsites, we can add
    // a static assert here to enforce T != Tensor
    return *static_cast<const T*>(pointer_);
  }

  const void* GetRaw() const noexcept {
    return pointer_;
  }
  void* GetRaw() noexcept {
    return pointer_;
  }

  /**
   * @brief Gets a mutable pointer to the stored object.
   *
   * If the current object is not of the right type, a new object is created
   * and the old object is freed. Note that type T should have a default
   * constructor. Otherwise, create the object yourself first, and use
   * Reset().
   */
  template <class T>
  T* GetMutable() {
    static_assert(
        std::is_default_constructible<T>::value,
        "GetMutable can't be called with non-default-constructible types. "
        "Try using specialized methods");
    static_assert(
        !std::is_same<T, Tensor>::value,
        "Use GetMutableTensor(DeviceType) instead");
    if (IsType<T>()) {
      return static_cast<T*>(pointer_);
    } else {
      VLOG(1) << "Create new mutable object " << TypeMeta::TypeName<T>();
      return Reset<T>(new T());
    }
  }

  template <class T>
  T* GetMutableOrNull() {
    if (IsType<T>()) {
      return static_cast<T*>(pointer_);
    } else {
      return nullptr;
    }
  }

  inline Tensor* GetMutableTensor(DeviceType device_type) {
    if (IsTensorType(device_type)) {
      return static_cast<Tensor*>(pointer_);
    } else {
      VLOG(1) << "Create new mutable object " << TypeMeta::TypeName<Tensor>()
              << " DeviceType:" << device_type;
      return Reset<Tensor>(new Tensor(device_type));
    }
  }

  /**
   * Sets the underlying object to the allocated one. The Blob then takes over
   * the ownership of the passed in pointer. If there is already an object in
   * the Blob, the old object is freed.
   *
   * This is used when the underlying class T does not have a default ctor, or
   * complex initializations needs to be done outside the blob.
   */
  template <class T>
  T* Reset(T* allocated) {
    free_();
    meta_ = TypeMeta::Make<T>();
    pointer_ = static_cast<void*>(allocated);
    has_ownership_ = true;
    return allocated;
  }

  /**
   * Sets the underlying object to the allocated one, but does not take over
   * the ownership of the passed in pointer. If there is already an object in
   * the Blob, the old object is freed.
   *
   * Unlike Reset, this does not take over the ownership of the pointer and the
   * caller is responsible for making sure that the lifetime of the allocated
   * blob outlasts the lifetime of any access to this blob, until another Reset
   * call is made or the blob is destructed.
   */
  template <class T>
  typename std::remove_const<T>::type* ShareExternal(
      typename std::remove_const<T>::type* allocated) {
    return static_cast<T*>(ShareExternal(
        static_cast<void*>(allocated),
        TypeMeta::Make<typename std::remove_const<T>::type>()));
  }

  void* ShareExternal(void* allocated, const TypeMeta& meta) {
    free_();
    meta_ = meta;
    pointer_ = static_cast<void*>(allocated);
    has_ownership_ = false;
    return allocated;
  }

  /**
   * Resets the Blob to an empty one.
   */
  inline void Reset() {
    free_();
    pointer_ = nullptr;
    meta_ = TypeMeta();
    has_ownership_ = false;
  }

  /**
   * Serializes the current blob, if possible. Note that this serialization uses
   * the registration mechanism and one has to implement specific serialization
   * approaches for specific classes. Acceptor should take care of writing data
   * to the actual storage.
   */
  void Serialize(
      const string& name,
      BlobSerializerBase::SerializationAcceptor acceptor,
      int chunk_size = kDefaultChunkSize) const;

  /**
   * @brief Convenience function to serialize a blob to a string.
   *
   * This is a conveinence function to serialize small Blobs that produce
   * manageable serialized strings. To serialize big blobs such as
   * large sparse tensors, use the fully-functional interface in
   * blob_serializer_base.h.
   *
   * NOTE: this function doesn't do chunking and might break with big tensors.
   */
  string Serialize(const string& name) const;

  /**
   * @brief Swaps the underlying storage of two blobs.
   */
  void swap(Blob& rhs) {
    using std::swap;
    swap(meta_, rhs.meta_);
    swap(pointer_, rhs.pointer_);
    swap(has_ownership_, rhs.has_ownership_);
  }

  /**
   * Deserializes from a string containing either BlobProto or TensorProto. If
   * the deserialization fails, the content in the blob should no longer be
   * trusted.
   */
  void Deserialize(const string& content);
  void Deserialize(const BlobProto& proto);

 private:
  void free_() {
   if (has_ownership_) {
     CAFFE_ENFORCE(pointer_ != nullptr, "Can't have ownership of nullptr");
     (*meta_.dtor())(pointer_, 1);
   }
  }

  TypeMeta meta_;
  void* pointer_ = nullptr;
  bool has_ownership_ = false;

  AT_DISABLE_COPY_AND_ASSIGN(Blob);
};

inline void swap(Blob& lhs, Blob& rhs) {
  lhs.swap(rhs);
}

inline std::ostream& operator<<(std::ostream & out, const Blob & v) {
  return out << "Blob";
}

}  // namespace caffe2
#endif  // CAFFE2_CORE_BLOB_H_
