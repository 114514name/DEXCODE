from bluedit import sceneio, model
p = sceneio.load_project("examples/gal/demo_blueprint.bluescene")
print("scenes:", len(p.scenes))
for si, sc in enumerate(p.scenes):
    print(f"Scene {si}: {sc.title}, nodes={len(sc.nodes)}")
    for n in sc.nodes:
        if n.type == model.N_CHOICE:
            print("Choice node id", n.id)
            print(" choice_text:", repr(n.choice_text))
            print(" options:", n.options)
            print(" outputs:", [(o.kind, o.name) for o in n.outputs])
            print("---")
print("done")
