# basic_json::operator+=

```cpp
// (1)
reference operator+=(basic_json&& val);
reference operator+=(const basic_json& val);

// (2)
reference operator+=(const typename object_t::value_type& val);

// (3)
reference operator+=(initializer_list_t init);
```

1. Appends the given element `val` to the end of the JSON array. If the function is called on a JSON null value, an
   empty array is created before appending `val`.

2. Inserts the given element `val` to the JSON object. If the function is called on a JSON null value, an empty object
   is created before inserting `val`.

3. This function allows to use `operator+=` with an initializer list. In case

    1. the current value is an object,
    2. the initializer list `init` contains only two elements, and
    3. the first element of `init` is a string,

    `init` is converted into an object element and added using `operator+=(const typename object_t::value_type&)`.
    Otherwise, `init` is converted to a JSON value and added using `operator+=(basic_json&&)`.

## Parameters

`val` (in)
:   the value to add to the JSON array/object

`init` (in)
:   an initializer list

## Return value

`#!cpp *this`

## Exceptions

1. The function can throw the following exceptions:
    - Throws [`type_error.308`](../../home/exceptions.md#jsonexceptiontype_error308) when called on a type other than
      JSON array or null; example: `"cannot use operator+=() with number"`
2. The function can throw the following exceptions:
    - Throws [`type_error.308`](../../home/exceptions.md#jsonexceptiontype_error308) when called on a type other than
      JSON object or null; example: `"cannot use operator+=() with number"`

## Complexity

1. Amortized constant.
2. Logarithmic in the size of the container, O(log(`size()`)).
3. Linear in the size of the initializer list `init`.

## Notes

(3) This function is required to resolve an ambiguous overload error, because pairs like `{"key", "value"}` can be both
interpreted as `object_t::value_type` or `std::initializer_list<basic_json>`, see
[#235](https://github.com/nlohmann/json/issues/235) for more information.

## Examples

??? example

    The example shows how `push_back()` and `+=` can be used to add elements to a JSON array. Note how the `null` value
    was silently converted to a JSON array.
    
    ```cpp
    --8<-- "examples/push_back.cpp"
    ```
    
    Output:
    
    ```json
    --8<-- "examples/push_back.output"
    ```

??? example

    The example shows how `push_back()` and `+=` can be used to add elements to a JSON object. Note how the `null` value
    was silently converted to a JSON object.

    ```cpp
    --8<-- "examples/push_back__object_t__value.cpp"
    ```
    
    Output:
    
    ```json
    --8<-- "examples/push_back__object_t__value.output"
    ```

??? example

    The example shows how initializer lists are treated as objects when possible.

    ```cpp
    --8<-- "examples/push_back__initializer_list.cpp"
    ```
    
    Output:
    
    ```json
    --8<-- "examples/push_back__initializer_list.output"
    ```

## Version history

1. Since version 1.0.0.
2. Since version 1.0.0.
2. Since version 2.0.0.
