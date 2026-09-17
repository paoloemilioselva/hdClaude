# A valueless attribute reaches Hydra as a zero, not as "no opinion"

A report prepared for OpenUSD, kept here so that the finding is not only a
comment in this renderer. It is written to be pasted into an issue; nothing in
it is specific to hdClaude except the observation that first exposed it.

**Filed:** not yet.
**Found:** 2026-09-16, while rendering a third-party asset whose lights lit a
room through its walls.

## Summary

`UsdImagingDataSourceAttribute<T>::GetTypedValue` returns a zero-initialised `T`
when an attribute has neither an authored value nor a schema fallback. For a
`bool` that zero is `false`, which is a *value* — and every Hydra render
delegate on the scene-index path receives it as though the scene had asked for
it. A delegate cannot distinguish "the scene says false" from "the scene says
nothing", so a property the author never gave an opinion about silently acquires
the most consequential opinion the type can hold.

The case where this bites hardest is `UsdLuxShadowAPI`'s `inputs:shadow:enable`,
whose documented fallback is `true`: a light that declares the property and
gives it no value becomes a light that casts no shadows at all.

## Reproduction

```usda
#usda 1.0
def SphereLight "declared_no_value"
{
    bool inputs:shadow:enable
}

def SphereLight "shadow_api_applied" (
    prepend apiSchemas = ["ShadowAPI"]
)
{
    bool inputs:shadow:enable
}
```

At the USD level the two are clearly different, and the first has no opinion of
any kind (OpenUSD 26.03, Windows):

```
declared_no_value    ShadowAPI=False HasAuthoredValue=False Get()=None  schema fallback=None
shadow_api_applied   ShadowAPI=True  HasAuthoredValue=False Get()=True  schema fallback=True
```

`Get()` returning `None` is the whole point: there is no value to be had. The
fallback of `true` belongs to `UsdLuxShadowAPI`, so a prim that does not apply
that schema has no fallback for the property either —
`GetAttributeFallbackValue` reports it as absent from the prim definition.

Through a scene index, the first light arrives at a render delegate with
`shadow:enable = false`. Observed in a Hydra render delegate that honours the
value it is given; the same data source serves every delegate on that path.

## Where it comes from

`pxr/usdImaging/usdImaging/dataSourceAttribute.h`:

```cpp
T GetTypedValue(HdSampledDataSource::Time shutterOffset) override
{
    // Zero-initialization for numerical types.
    T result{};
    if (!_usdAttrQuery) {
        return result;
    }
    ...
    bool valueRetrieved = _usdAttrQuery.Get<T>(&result, time);
    if (valueRetrieved) {
        return result;
    }

    // No value of expected type T could be retrieved. Try schema default
    // and if that fails return zero-initialized result.
    valueRetrieved = _usdAttrQuery.GetFallbackValue<T>(&result);
    if (valueRetrieved) {
        return result;
    }

    return result;
}
```

Both retrieval paths can fail, and the function has no way to say so: its return
type is `T`. The comment is candid about it, and for a `float` the resulting
zero is usually harmless. For a `bool` whose schema fallback is `true`, and for
an enum-like token, the zero is the opposite of what the specification says.

## Why the caller cannot work around it

A delegate sees a data source that answers `false`. It has no way to ask whether
the answer came from the scene, from a schema, or from the zero-initialiser.
Working around it would mean inventing a rule — "ignore a `false` for
`shadow:enable` unless `ShadowAPI` is applied" — which is guesswork that would
then be wrong for a light that authored `false` deliberately.

The information is not recoverable downstream. It exists only at the point where
`Get` and `GetFallbackValue` both return false.

## Suggested directions

1. **Do not create the data source when there is no opinion.** If both
   retrievals fail, `UsdImagingDataSourceAttribute` could be absent from its
   container rather than present and zero. A delegate then sees "no opinion",
   which is what Hydra's container model already expresses, and applies its own
   default — the same thing it does for a property nobody authored. This looks
   closest to the existing design, since the two cases are genuinely identical
   from the scene's point of view.
2. **Consult the schema registry for the property's fallback**, not only the
   attribute query's, so a documented fallback is honoured even when the prim
   has no applied schema supplying it. This changes what a delegate receives
   rather than whether it receives anything, and it would need care about which
   schema's fallback applies.
3. At minimum, **document it**: that a data source may be present with a
   zero-initialised value, and that delegates cannot distinguish that from an
   authored one.

Option 1 is the one this renderer would prefer, because it makes the invalid
case visible instead of plausible.

## Impact

Assets in the wild do author valueless properties: the OpenPBR Shader Playground
declares `bool inputs:shadow:enable` and `color3f inputs:shadow:color` on three
of its lights, applying no `ShadowAPI`. Rendered honestly, a third of that
scene's light arrives through its walls — the frame's mean falls from 0.260 to
0.169 once the lights shadow as UsdLux specifies. Nothing errors, and nothing in
the image says the cause is a schema that was never applied.

The same mechanism applies to every valueless `bool` whose schema fallback is
`true`, and to any attribute whose zero is meaningful rather than neutral.
