# Release checklist

1. check that code compiles on all platforms (via github actions)
2. update documentation (if necessary)
3. update changelog (using git log)
4. update plugin.json:
    1. update tags in plugin.json (if necessary)
    2. update changelog link in plugin.json
    3. increate version in plugin.json
7. git tag -> auto build via github actions
8. update issue in vcv library repo