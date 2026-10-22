#
# The Hydra schema a cull volume is published with, in the definition format
# of OpenUSD's hdGenSchema. Regenerate cullVolumeSchema.h/.cpp from this
# directory with:
#
#   hdGenSchema --schemaFile hdSchemaDefs.py --all
#
# The class is named as it would be if the proposal were accepted into
# pxr/imaging/hd; until then it lives in this library.
#
[
    dict(
        SCHEMA_NAME = 'ALL_SCHEMAS',
        LIBRARY_PATH = 'usdImagingCullVolume',
        LIBRARY_NAME = 'UsdImagingCullVolume',
        HEADER_GUARD = 'usdImagingCullVolume',
    ),

    #--------------------------------------------------------------------------
    # cullVolume
    dict(
        SCHEMA_NAME = 'CullVolume',
        SCHEMA_CLASS_NAME = 'HdCullVolumeSchema',
        SCHEMA_TOKEN = 'cullVolume',
        ADD_DEFAULT_LOCATOR = True,
        DOC = '''
            The shape and behaviour of a cull volume (UsdCullVolume's
            CubeCullVolume, SphereCullVolume and PlaneCullVolume). Its
            transform, visibility and purpose are the prim's ordinary xform,
            visibility and purpose data sources, and the geometry it culls is
            the collection at collections/cullLink.''',
        MEMBERS = [
            ('ALL_MEMBERS', '', dict(ADD_LOCATOR = True)),
            ('shape', T_TOKEN,
             dict(DOC = 'cube, sphere or plane.')),
            ('size', T_DOUBLE,
             dict(DOC = 'Edge length of a cube, as UsdGeomCube.size.')),
            ('radius', T_DOUBLE,
             dict(DOC = 'Radius of a sphere, as UsdGeomSphere.radius.')),
            ('axis', T_TOKEN,
             dict(DOC = 'Normal of a plane, X, Y or Z; its inside is the '
                        'negative side.')),
            ('invert', T_BOOL,
             dict(DOC = 'Cull outside the shape instead of inside it.')),
            ('affectsPrimaryRays', T_BOOL, {}),
            ('affectsSecondaryRays', T_BOOL, {}),
        ],
        STATIC_TOKEN_DATASOURCE_BUILDERS = [
            ('shape', ['cube', 'sphere', 'plane']),
            ('axis', ['X', 'Y', 'Z']),
        ],
    ),
]
