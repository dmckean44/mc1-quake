# mdllod

Builds reduced-polygon MDL models from your own Quake pak files (the models
are id Software's, so generate them locally rather than shipping them).

    # replace models outright, keeping view weapons at 75%
    python3 mdllod.py id1/pak0.pak id1/pak1.pak -o id1/lowpoly.pak -r 0.5 --rule 'progs/v_*=0.75'

    # distant-model LODs, used when 64 world units cover fewer than r_lodsize pixels
    python3 mdllod.py id1/pak0.pak id1/pak1.pak -o id1/lod.pak -r 0.3 --suffix _lod \
        --min-normal-dot -0.5 --uv-tolerance 32 --rule 'progs/v_*=1'

    # compare a model: original, lowpoly, lod
    python3 preview.py id1/pak0.pak id1/lowpoly.pak id1/lod.pak --suffix '' --suffix _lod \
        --model progs/soldier.mdl -o soldier.png

The engine loads `lowpoly.pak` and `lod.pak` after the numbered paks. Like any
other added pak, they need the registered game. Needs Python 3 and numpy.
