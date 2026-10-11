# Frontière d'extraction du PPU CGB

Le pipeline commun DMG/CGB reste dans `cores/gb/src/ppu` : fetcher BG/fenêtre,
FIFO BG et OBJ, scan OAM et cadence des modes LCD ne sont pas dupliqués.

Les différences couleur sont sélectionnées par le mode matériel explicite :
banque VRAM, attributs et palettes CGB, priorité `OPRI` et comportement de
compatibilité. Les portes CPU VRAM/OAM/CRAM restent dans le pipeline commun et
suivent sa phase interne, tandis que les registres d'index CGB demeurent
accessibles en mode 3 et s'auto-incrémentent même lorsque l'écriture de données
palette est rejetée. Ce dossier recevra les stratégies réellement propres à
une révision CGB lorsqu'elles pourront être extraites sans créer de dépendance
cyclique avec le matériel commun.
