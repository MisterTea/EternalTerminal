# Instructions to creating a new ET Release

1. Increment the project(...) version in CMakeLists.txt and push to master.
2. Switch to the release branch
3. Pull in the latest changes: `git merge master`
4. Run `git submodule update --recursive --init` to ensure that the submodules are updated to the latest master/release commit.
5. Run `import_submodules.sh` to create an in-repo copy of the submodules
6. `git add external_imported` and commit/push any changes
7. Create a github release using the latest commit and tag it with `et-vA.B.C` where A/B/C is the major/minor/patch version.
8. Switch to the deployment branch
9. Edit the `deployment/debian_SOURCE/changelog` file and add a entry for the new release.
10. Copy id_rsa / id_rsa.pub / .gnupg to the deployment directory.  The rsa key needs to have write access to the debian-et github repo and the gnupg key needs to have write access to launchpad
11. Build and run a docker image interactively from ubuntu.Dockerfile
12. inside the docker image, run deploy_ubuntu_ppa.sh
13. Exit the docker image and create a vagrant VM for debian
14. inside the vagrant vm, run build_all_deb.sh
15. Follow the instructions at the end of the build_all_deb command to push the new debian artifacts.  Do not push the dbgsym (debug symbols) because github cannot handle files that large
