/*
 * Copyright 2004-2019 Cray Inc.
 * Other additional copyright holders may be indicated within.
 *
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
   This module contains Chapel types that can serve as C pointer
   types for the purposes of interoperability and methods to work
   with them.

   See also :ref:`readme-extern`.
 */
module CPtr {
  use ChapelStandard;

  /* A Chapel version of a C NULL pointer. */
  extern const c_nil:c_void_ptr;

  // To generate legal C prototypes, we have to manually instantiate this
  // prototype for each pointer type that might be associated with 'x'.

  /* :returns: true if the passed value is a NULL pointer (ie 0) */
  pragma "no prototype"
  pragma "no doc"
  extern proc is_c_nil(x):bool;

  /*

    Represents a local C pointer for the purpose of C integration. This class
    represents the equivalent to a C language pointer. Instances of this class
    support assignment to other instances or nil, == or != comparison with a
    ``c_void_ptr`` or with ``nil``, and casting to another ``c_ptr`` type or to
    the ``c_void_ptr`` type.

    As with a Chapel class, a ``c_ptr`` can be tested non-nil simply
    by including it in an if statement conditional, like so:

    .. code-block:: chapel

      var x: c_ptr = c_ptrTo(...);
      if x then do writeln("x is not nil");
      if !x then do writeln("x is nil");

    Additionally, a ``c_ptr`` can be output like so:

    .. code-block:: chapel

      var x: c_ptr = c_ptrTo(...);
      writeln(x); // outputs nil or e.g. 0xabc123000000

  */

  //   Similar to _ddata from ChapelBase, but differs
  //   from _ddata because it can never be wide.
  pragma "data class"
  pragma "no object"
  pragma "no default functions"
  pragma "no wide class"
  pragma "c_ptr class"
  class c_ptr {
    /* The type that this pointer points to */
    type eltType;
    /* Retrieve the i'th element (zero based) from a pointer to an array.
      Does the equivalent of ptr[i] in C.
    */
    inline proc this(i: integral) ref {
      return __primitive("array_get", this, i);
    }
    /* Get element pointed to directly by this pointer. If the pointer
      refers to an array, this will return ptr[0].
    */
    inline proc deref() ref {
      return __primitive("array_get", this, 0);
    }
    /* Print this pointer */
    inline proc writeThis(ch) {
      (this:c_void_ptr).writeThis(ch);
    }
  }

  /*
  This class represents a C array with fixed size.  A variable of type c_array
  can coerce to a c_ptr with the same element type.  In that event, the
  pointer will be equivalent to `c_ptrTo(array[0])`.  A c_array behaves
  similarly to a homogenous tuple except that its indices start at 0 and it is
  guaranteed to be stored in contiguous memory.  A c_array variable has value
  semantics. Declaring one as a function local variable will create the array
  elements in the function's stack. Assigning or copy initializing will result
  in copying the elements (vs resulting in two pointers that refer to the same
  elements).  A `nil` c_array is not representable in Chapel.
  */
  //   Similar to c_ptr but includes a param width
  //   (matching a C fixed-size array).
  //pragma "data class"
  //pragma "no object"
  //pragma "no default functions"
  //pragma "no wide class"
  //pragma "c_ptr class"
  pragma "c_array record"
  record c_array {
    /* The array element type */
    type eltType;
    /* The fixed number of elements */
    param size;

    /*pragma "no doc"
    pragma "no codegen"
    pragma "no init field"
    var x1: eltType; // this field keeps type alive until code generation
     */

    proc init(type eltType, param size) {
      this.eltType = eltType;
      this.size = size;
    }

    /* Retrieve the i'th element (zero based) from the array.
      Does the equivalent of arr[i] in C.
        Includes bounds checking when such checks are enabled.
    */
    inline proc this(i: integral) ref {
      if boundsChecking then
        if i < 0 || i >= size then
          HaltWrappers.boundsCheckHalt("c array index out of bounds " + i +
                                       "(indices are 0.." + (size-1) + ")");

      return __primitive("array_get", this, i);
    }
    /* As with the previous function, returns the i'th element (zero based)
        from the array. This one emits a compilation error if i is out of bounds.
    */
    inline proc this(param i: integral) ref {
      if i < 0 || i >= size then
        compilerError("c array index out of bounds " + i +
                      "(indices are 0.." + (size-1) + ")");

      return __primitive("array_get", this, i);
    }

    /* Print the elements */
    proc writeThis(ch) {
      /*if __primitive("ptr_eq", this, nil) {
        ch <~> new ioLiteral("nil");
        return;
      }*/

      ch <~> new ioLiteral("[");
      var first = true;
      /*for i in 0..#size {

        ch <~> this(i);

        if i != size-1 then
          ch <~> new ioLiteral(", ");
      }*/
      ch <~> new ioLiteral("]");
    }

    /*proc init(other: c_array) {
      // TODO: size mismatch error
      // TODO: eltType mismatch error
      //if other.size != this.size then
      //  compilerError("Size mismatch in c array initialization");

      this.eltType = other.eltType;
      this.size = other.size;
      for i in 0..#size {
        this[i] = other[i];
      }
    }*/
  }

  /* Copy the elements from one c_array to another.
     Raises an error at compile time if the array sizes or
     element types do not match. */
  proc =(ref lhs:c_array, rhs:c_array) {
    if lhs.size != rhs.size then
      compilerError("Size mismatch in c array assignment");

    for i in 0..#lhs.size {
      lhs[i] = rhs[i];
    }
  }

  pragma "no doc"
  inline proc c_void_ptr.writeThis(ch) {
    try {
      ch.writef("0x%xu", this:c_uintptr);
    } catch e: SystemError {
      ch.setError(e.err);
    } catch {
      ch.setError(EINVAL:syserr);
    }
  }

  pragma "no doc"
  inline proc =(ref a:c_ptr, b:c_ptr) { __primitive("=", a, b); }

  pragma "no doc"
  inline proc =(ref a: c_ptr, b: c_void_ptr) { __primitive("=", a, b); }

  pragma "no doc"
  inline proc =(ref a:c_ptr, b:_nilType) { __primitive("=", a, c_nil); }

  pragma "no doc"
  inline proc _cast(type t:c_ptr, x:_nilType) {
    return __primitive("cast", t, x);
  }
  pragma "no doc"
  inline proc _cast(type t:c_void_ptr, x:_nilType) {
    return c_nil;
  }


  pragma "no doc"
  inline proc _cast(type t:c_ptr, x:c_ptr) {
    return __primitive("cast", t, x);
  }
  pragma "no doc"
  inline proc _cast(type t:c_array, x:c_ptr) {
    return __primitive("cast", t, x);
  }
  pragma "no doc"
  inline proc _cast(type t:c_void_ptr, x:c_ptr) {
    return __primitive("cast", t, x);
  }
  pragma "no doc"
  inline proc _cast(type t:c_ptr, x:c_void_ptr) {
    return __primitive("cast", t, x);
  }
  pragma "no doc"
  inline proc _cast(type t:string, x:c_void_ptr) {
    return new string(__primitive("ref to string", x), needToCopy=false);
  }
  pragma "no doc"
  inline proc _cast(type t:string, x:c_ptr) {
    return new string(__primitive("ref to string", x), needToCopy=false);
  }
  pragma "no doc"
  inline proc _cast(type t:borrowed, x:c_void_ptr) {
    return __primitive("cast", t, x);
  }
  pragma "no doc"
  inline proc _cast(type t:unmanaged, x:c_void_ptr) {
    return __primitive("cast", t, x);
  }
  pragma "no doc"
  inline proc _cast(type t:c_void_ptr, x:borrowed) {
    return __primitive("cast", t, x);
  }
  pragma "no doc"
  inline proc _cast(type t:c_void_ptr, x:unmanaged) {
    return __primitive("cast", t, x);
  }

  pragma "no doc"
  inline proc _cast(type t:_ddata, x:c_ptr) where t.eltType == x.eltType {
    return __primitive("cast", t, x);
  }
  pragma "no doc"
  inline proc _cast(type t:_ddata, x:c_void_ptr) {
    return __primitive("cast", t, x);
  }
  pragma "no doc"
  inline proc _cast(type t:c_void_ptr, x:_ddata) {
    return __primitive("cast", t, x);
  }

  pragma "no doc"
  inline proc _cast(type t:c_intptr, x:c_void_ptr)
    return __primitive("cast", t, x);
  pragma "no doc"
  inline proc _cast(type t:c_uintptr, x:c_void_ptr)
    return __primitive("cast", t, x);
  pragma "no doc"
  inline proc _cast(type t:int(64), x:c_void_ptr) where c_uintptr != int(64)
    return __primitive("cast", t, x);
  pragma "no doc"
  inline proc _cast(type t:uint(64), x:c_void_ptr) where c_uintptr != uint(64)
    return __primitive("cast", t, x);

  pragma "no doc"
  inline proc _cast(type t:c_intptr, x:c_ptr)
    return __primitive("cast", t, x);
  pragma "no doc"
  inline proc _cast(type t:c_uintptr, x:c_ptr)
    return __primitive("cast", t, x);
  pragma "no doc"
  inline proc _cast(type t:int(64), x:c_ptr) where c_intptr != int(64)
    return __primitive("cast", t, x);
  pragma "no doc"
  inline proc _cast(type t:uint(64), x:c_ptr) where c_uintptr != int(64)
    return __primitive("cast", t, x);

  pragma "compiler generated"
  pragma "last resort"
  pragma "no doc"
  inline proc _defaultOf(type t:c_void_ptr) {
      return __primitive("cast", t, nil);
  }

  pragma "compiler generated"
  pragma "last resort"
  pragma "no doc"
  inline proc _defaultOf(type t:c_ptr) {
      return __primitive("cast", t, nil);
  }

  pragma "compiler generated"
  pragma "last resort"
  pragma "no doc"
  inline proc _defaultOf(type t:c_fn_ptr) {
      return __primitive("cast", t, nil);
  }

  pragma "no doc"
  inline proc =(ref a:c_fn_ptr, b:_nilType) { __primitive("=", a, c_nil); }

  pragma "no doc"
  inline proc =(ref a:c_fn_ptr, b:c_fn_ptr) { __primitive("=", a, b); }



  pragma "no doc"
  inline proc ==(a: c_ptr, b: c_ptr) where a.eltType == b.eltType {
    return __primitive("ptr_eq", a, b);
  }
  pragma "no doc"
  inline proc ==(a: c_ptr, b: c_void_ptr) {
    return __primitive("ptr_eq", a, b);
  }
  pragma "no doc"
  inline proc ==(a: c_void_ptr, b: c_ptr) {
    return __primitive("ptr_eq", a, b);
  }
  pragma "no doc"
  inline proc ==(a: c_ptr, b: _nilType) {
    return __primitive("ptr_eq", a, c_nil);
  }
  pragma "no doc"
  inline proc ==(a: _nilType, b: c_ptr) {
    return __primitive("ptr_eq", c_nil, b);
  }
  pragma "no doc"
  inline proc ==(a: c_void_ptr, b: _nilType) {
    return __primitive("ptr_eq", a, c_nil);
  }
  pragma "no doc"
  inline proc ==(a: _nilType, b: c_void_ptr) {
    return __primitive("ptr_eq", c_nil, b);
  }

  pragma "no doc"
  inline proc !=(a: c_ptr, b: c_ptr) where a.eltType == b.eltType {
    return __primitive("ptr_neq", a, b);
  }
  pragma "no doc"
  inline proc !=(a: c_ptr, b: c_void_ptr) {
    return __primitive("ptr_neq", a, b);
  }
  pragma "no doc"
  inline proc !=(a: c_void_ptr, b: c_ptr) {
    return __primitive("ptr_neq", a, b);
  }
  pragma "no doc"
  inline proc !=(a: c_ptr, b: _nilType) {
    return __primitive("ptr_neq", a, c_nil);
  }
  pragma "no doc"
  inline proc !=(a: _nilType, b: c_ptr) {
    return __primitive("ptr_neq", c_nil, b);
  }
  pragma "no doc"
  inline proc !=(a: c_void_ptr, b: _nilType) {
    return __primitive("ptr_neq", a, c_nil);
  }
  pragma "no doc"
  inline proc !=(a: _nilType, b: c_void_ptr) {
    return __primitive("ptr_neq", c_nil, b);
  }

  pragma "no doc"
  inline proc _cond_test(x: c_ptr) return x != c_nil;

  pragma "no doc"
  inline proc !(x: c_ptr) return x == c_nil;

  pragma "no doc"
  inline proc +(a: c_ptr, b: integral) return __primitive("+", a, b);

  pragma "no doc"
  inline proc -(a: c_ptr, b: integral) return __primitive("-", a, b);


  pragma "no doc"
  extern proc c_pointer_return(ref x:?t):c_ptr(t);


  /*

    Returns a :type:`c_ptr` to a Chapel rectangular array.  Note that the
    existence of this :type:`c_ptr` has no impact on the lifetime of the array.
    The returned pointer will be invalid if the original array is freed or even
    reallocated. Domain assignment could make this :type:`c_ptr` invalid. If
    the array's data is stored in more than one chunk the procedure will halt
    the program with an error message.

    :arg arr: the array for which we should retrieve a pointer
    :returns: a pointer to the array data
  */
  inline proc c_ptrTo(arr: []) where isRectangularArr(arr) {
    return c_pointer_return(arr[arr.domain.low]);
  }

  /* Returns a :type:`c_ptr` to any Chapel object.
    Note that the existence of the :type:`c_ptr` has no impact of the lifetime
    of the object. In many cases the object will be stack allocated and
    could go out of scope even if this :type:`c_ptr` remains.

    :arg x: the by-reference argument to get a pointer to. The argument should
            not be an array or domain (there is a different overload for arrays).
            Records, class instances, integral, real, imag, and complex types are
            supported.
    :returns: a pointer to the argument passed by reference

  */
  inline proc c_ptrTo(ref x:?t):c_ptr(t) {
    if isArrayType(t) then
      compilerError("c_ptrTo unsupported array type", 2);
    if isDomainType(t) then
      compilerError("c_ptrTo domain type not supported", 2);
    // Other cases should be avoided, e.g. sync vars
    return c_pointer_return(x);
  }

  pragma "no doc"
  inline proc c_ptrTo(x: c_fn_ptr) {
    return x;
  }
  pragma "no doc"
  proc c_fn_ptr.this() {
    compilerError("Can't call a C function pointer within Chapel");
  }
  pragma "no doc"
  proc c_fn_ptr.this(args...) {
    compilerError("Can't call a C function pointer within Chapel");
  }


  // Offset the CHPL_RT_MD constant in order to preserve the value through
  // calls to chpl_here_alloc. See comments on offset_STR_* in String.chpl
  // for more.
  private proc offset_ARRAY_ELEMENTS {
    extern const CHPL_RT_MD_ARRAY_ELEMENTS:chpl_mem_descInt_t;
    extern proc chpl_memhook_md_num(): chpl_mem_descInt_t;
    return CHPL_RT_MD_ARRAY_ELEMENTS - chpl_memhook_md_num();
  }

  /*
    Return the size in bytes of a type, as with the C ``sizeof`` built-in.

    .. warning::

      This method is intended for C interoperability. To enhance
      flexibility, it is possible to request the sizes of Chapel types.
      However, be aware:

         * Chapel types are not necessarily stored in contiguous memory
         * Behavior of ``c_sizeof`` with Chapel types may change
         * Behavior given a Chapel class type is not well-defined
   */
  inline proc c_sizeof(type x): size_t {
    extern proc sizeof(type x): size_t;
    return sizeof(x);
  }

  /*
    Return the offset of a field in a record.

    .. warning::

      This method is intended for C interoperability.  To enhance flexibility,
      it is possible to request the offset of elements within a Chapel record.
      However, be aware:

      * Chapel types are not necessary stored in contiguous memory
      * Behavior of ``c_offsetof`` may change
      * Behavior given a Chapel class type field is not well-defined
   */
  proc c_offsetof(type t, param fieldname : string): size_t where isRecordType(t) {
    use Reflection;
    var x: t;

    return c_ptrTo(getFieldRef(x, fieldname)):size_t - c_ptrTo(x):size_t;
  }

  pragma "no doc"
  proc c_offsetof(type t, param fieldname: string) where !isRecordType(t) {
    compilerError("Cannot call c_offsetof on type that is not a record");
  }

  /*
    Allocate memory and initialize all bits to 0. Note that this simply zeros
    memory, it does not call Chapel initializers (it is meant for primitive
    types and C interoperability only.) This memory should eventually be freed
    with :proc:`c_free`.

    :arg eltType: the type of the elements to allocate
    :arg size: the number of elements to allocate space for
    :returns: a c_ptr(eltType) to allocated memory
    */
  inline proc c_calloc(type eltType, size: integral) : c_ptr(eltType) {
    const alloc_size = size.safeCast(size_t) * c_sizeof(eltType);
    return chpl_here_calloc(alloc_size, 1, offset_ARRAY_ELEMENTS):c_ptr(eltType);
  }

  /*
    Allocate memory that is not initialized. This memory should eventually be
    freed with :proc:`c_free`.

    :arg eltType: the type of the elements to allocate
    :arg size: the number of elements to allocate space for
    :returns: a c_ptr(eltType) to allocated memory
    */
  inline proc c_malloc(type eltType, size: integral) : c_ptr(eltType) {
    const alloc_size = size.safeCast(size_t) * c_sizeof(eltType);
    return chpl_here_alloc(alloc_size, offset_ARRAY_ELEMENTS):c_ptr(eltType);
  }

  /* Free memory that was allocated with :proc:`c_calloc` or :proc:`c_malloc`.

    :arg data: the c_ptr to memory that was allocated. Note that both
               `c_ptr(t)` and `c_void_ptr` can be passed to this argument.
    */
  inline proc c_free(data: c_void_ptr) {
    chpl_here_free(data);
  }

  /* Returns true if t is a c_ptr type or c_void_ptr.
   */
  proc isAnyCPtr(type t:c_ptr) param return true;
  pragma "no doc"
  proc isAnyCPtr(type t:c_void_ptr) param return true;
  pragma "no doc"
  proc isAnyCPtr(type t) param return false;

  /*
    Copies n potentially overlapping bytes from memory area src to memory
    area dest.

    This is a simple wrapper over the C ``memmove()`` function.

    :arg dest: the destination memory area to copy to
    :arg src: the source memory area to copy from
    :arg n: the number of bytes from src to copy to dest
   */
  inline proc c_memmove(dest:c_void_ptr, const src:c_void_ptr, n: integral) {
    extern proc memmove(dest: c_void_ptr, const src: c_void_ptr, n: size_t);
    memmove(dest, src, n.safeCast(size_t));
  }

  /*
    Copies n non-overlapping bytes from memory area src to memory
    area dest. Use :proc:`c_memmove` if memory areas do overlap.

    This is a simple wrapper over the C memcpy() function.

    :arg dest: the destination memory area to copy to
    :arg src: the source memory area to copy from
    :arg n: the number of bytes from src to copy to dest
   */
  inline proc c_memcpy(dest:c_void_ptr, const src:c_void_ptr, n: integral) {
    extern proc memcpy (dest: c_void_ptr, const src: c_void_ptr, n: size_t);
    memcpy(dest, src, n.safeCast(size_t));
  }

  /*
    Compares the first n bytes of memory areas s1 and s2

    This is a simple wrapper over the C ``memcmp()`` function.

    :returns: returns an integer less than, equal to, or greater than zero if
              the first n bytes of s1 are found, respectively, to be less than,
              to match, or be greater than the first n bytes of s2.
   */
  inline proc c_memcmp(const s1:c_void_ptr, const s2:c_void_ptr, n: integral) {
    extern proc memcmp(const s1: c_void_ptr, const s2: c_void_ptr, n: size_t) : c_int;
    return memcmp(s1, s2, n.safeCast(size_t)).safeCast(int);
  }

  /*
    Fill bytes of memory with a particular byte value.

    This is a simple wrapper over the C ``memset()`` function.

    :arg s: the destination memory area to fill
    :arg c: the byte value to use
    :arg n: the number of bytes of b to fill

    :returns: s
   */
  inline proc c_memset(s:c_void_ptr, c:integral, n: integral) {
    extern proc memset(s: c_void_ptr, c: c_int, n: size_t) : c_void_ptr;
    memset(s, c.safeCast(c_int), n.safeCast(size_t));
    return s;
  }

}
